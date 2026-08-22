#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>

#include "OrderBook.h"
#include "MatchingEngine.h"
#include "MarketMaker.h"
#include "Strategy.h"
#include "RandomTrader.h"
#include "HistoricalDataReplay.h"

// Everything the top control bar / summary panel needs to configure a run.
// Strategy is passed separately to the constructor, not stored here - it's
// polymorphic and owned by the caller, same as MarketMaker already assumes.
struct SimConfig {
    enum class Source { Synthetic, Historical };

    Source      source   = Source::Historical;
    std::string dataPath;          // used when source == Historical
    int64_t     maxTicks = 1000;
    unsigned    seed     = 42;     // used when source == Synthetic (RandomTrader)

    // These differ wildly between sources today (BTC-scale historical numbers are
    // ~1,000,000x the synthetic ~$100 defaults, per the quantity-scaling decision) -
    // main.cpp hardcoded two different sets rather than one shared default. Set them
    // explicitly per-run instead of relying on a single default that can't fit both.
    int64_t quoteSize    = 10;
    int64_t maxInventory = 100;
    int64_t maxOrderSize = 50;
    double  maxLoss      = 1000.0;
    double  maxExposure  = 5000.0;
};

// One frame of state. Cheap to copy - the eventual GUI reads it every frame,
// a recorder would write it every tick. Still trimmed from the full proposal:
// no queue position, book imbalance, or markouts - those need modelling work
// that hasn't been built. bidLevels/askLevels below is no longer trimmed,
// now that OrderBook's map-based structure (Phase 5.1) makes depth cheap to
// expose.
struct SimSnapshot {
    int64_t tick = 0;

    double mid      = 0.0;
    double bestBid   = 0.0;
    double bestAsk   = 0.0;
    double spread    = 0.0;

    // Top-of-book depth ladder, best price first on each side. Aggregated
    // per price level (total resting quantity + order count), not per
    // individual order - a GUI wanting to highlight ITS OWN resting orders
    // does that separately, by comparing myBid/myAsk below against each
    // level's price.
    std::vector<BookLevel> bidLevels;
    std::vector<BookLevel> askLevels;

    double myBid = 0.0;   // 0.0 if not currently quoting that side
    double myAsk = 0.0;
    double fairValue = 0.0; // MarketMaker's own EWMA fair-value estimate

    // Fill events THIS tick specifically (not cumulative - fillCount below is
    // cumulative). Only the most recent fill price if more than one trade
    // happened in a single tick (e.g. an incoming order walking multiple
    // price levels) - good enough for a "mark where a fill happened" scatter
    // point, not a full per-trade log.
    bool hadFillThisTick = false;
    double lastFillPrice = 0.0;
    int64_t lastFillQty = 0;

    int64_t inventory      = 0;
    int64_t inventoryLimit = 0;

    double cash          = 0.0;
    double realisedPnL   = 0.0;
    double unrealisedPnL = 0.0;
    double totalPnL      = 0.0;
    double spreadPnL      = 0.0;
    double inventoryPnL   = 0.0;
    double exposure        = 0.0;
    double adverseSelectionRatio = 0.0;
    int64_t fillCount = 0; // cumulative fills so far this run - drives fill rate / spread capture
    int64_t buyFillCount = 0;  // subset of fillCount attributable to a filled bid
    int64_t sellFillCount = 0; // subset of fillCount attributable to a filled ask
    int64_t totalFilledVolume = 0; // cumulative filled quantity (both sides) - drives the inventory "turns" readout

    bool killSwitchActive = false;
};

// Wraps the existing OrderBook/MatchingEngine/MarketMaker plumbing behind a
// step()/snapshot() interface instead of a closed for-loop, so the same
// engine can be driven by a CLI while-loop, a recorder, or (later) a GUI
// render loop without duplicating the simulation logic in each.
class SimulationEngine {
public:
    SimulationEngine(SimConfig config, Strategy& strategy);

    // Advances exactly one tick (one quote + one incoming order). Returns
    // false once the feed is exhausted (historical) or maxTicks is reached
    // (either source) - the caller's while-loop condition.
    bool step();

    const SimSnapshot& snapshot() const;

    // True for Synthetic (nothing to open) or Historical with the data file
    // successfully opened. False means config.dataPath couldn't be opened -
    // step() will still return false on the first call either way, but this
    // is what lets a caller show "file not found" instead of silently
    // looking like an empty/instantly-exhausted feed.
    bool isDataSourceReady() const;

private:
    void refreshSnapshot();

    SimConfig config;
    Strategy& strategy;

    OrderBook orderBook;
    MatchingEngine matchingEngine;
    MarketMaker marketMaker;

    // Exactly one of these is constructed, matching config.source. Both need
    // dynamic allocation because neither RandomTrader nor HistoricalDataReplay
    // is default-constructible (both take required constructor arguments), so
    // an engine running the other source can't carry a real instance of the
    // one it doesn't need.
    std::unique_ptr<RandomTrader> randomTrader;
    std::unique_ptr<HistoricalDataReplay> replay;

    int64_t currentTick = 0;
    SimSnapshot latestSnapshot;

    // Set by the onTrade callback (constructor), read and reset once per
    // step() - see the comment there for why this needs to be reset at the
    // START of each tick rather than after refreshSnapshot().
    bool tickHadFill = false;
    double tickLastFillPrice = 0.0;
    int64_t tickLastFillQty = 0;
};
