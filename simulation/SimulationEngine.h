#pragma once

#include <memory>
#include <queue>
#include <random>
#include <string>
#include <cstdint>
#include <vector>

#include "OrderBook.h"
#include "MatchingEngine.h"
#include "MarketMaker.h"
#include "Strategy.h"
#include "RandomTrader.h"
#include "HistoricalDataReplay.h"
#include "SimEvent.h"

// base + Exp(jitterNs) delay, in nanoseconds. Defaults to always-zero, so a
// SimConfig that never sets these behaves exactly as if latency didn't exist -
// existing callers (main.cpp, the GUI, tests) don't need to change anything.
// Exponential jitter (not uniform/constant) gives the occasional large spike
// a constant delay can't - "the tail that hurts" per the desk review this
// design comes from.
struct LatencyModel {
    double baseNs = 0.0;
    double jitterNs = 0.0; // mean of the exponential jitter added on top of baseNs
    int64_t sample(std::mt19937& rng) const;
};

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

    // All default to zero delay - latency is opt-in. See LatencyModel above
    // for why exponential jitter, and SimulationEngine's event-queue comments
    // for what each one actually delays.
    LatencyModel latencyIn;     // the strategy decides against a state this old
    LatencyModel latencyOut;    // a decided quote takes this long to reach the book
    LatencyModel latencyCancel; // a decided cancel takes this long to reach the book

    // Optional companion to maxTicks: once step() is one EVENT (not one
    // quote-and-match round - see the class comment), the same maxTicks value
    // represents a different, usually much shorter, amount of simulated
    // activity than before. 0 = unbounded (rely on maxTicks / feed exhaustion
    // only); set this to bound a run by simulated wall-clock time instead.
    int64_t maxSimTimeNs = 0;
};

// One frame of state. Cheap to copy - the eventual GUI reads it every frame,
// a recorder would write it every tick. Still trimmed from the full proposal:
// no queue position, book imbalance, or markouts - those need modelling work
// that hasn't been built. bidLevels/askLevels below is no longer trimmed,
// now that OrderBook's map-based structure (Phase 5.1) makes depth cheap to
// expose.
struct SimSnapshot {
    int64_t tick = 0;
    int64_t timestampNs = 0; // real tape time (Historical) or synthetic clock (Synthetic); 0 if unavailable

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

    // Whether that fill was (one of) THIS MarketMaker's own resting orders -
    // real id-based attribution (see SimulationEngine's onTrade wiring), not
    // the price-match heuristic a GUI would otherwise have to re-derive.
    bool lastFillWasMine = false;
    OrderSide lastFillMineSide = OrderSide::BUY; // meaningful only if lastFillWasMine

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
// engine can be driven by a CLI while-loop, a recorder, or a GUI render loop
// without duplicating the simulation logic in each.
//
// Internally driven by a timestamp-ordered priority queue of SimEvents, not a
// synchronous "quote, then match one order" round - this is what makes
// latency representable at all. step() now advances exactly ONE EVENT, not
// one full round, so a given SimConfig::maxTicks value covers a different
// (usually much shorter) amount of simulated activity than it did before this
// changed - see maxSimTimeNs on SimConfig for a companion time-based bound.
//
// Latency is applied by NOT executing an effect immediately when it's
// decided, but scheduling a later event for it instead:
//   - latencyIn: a MarketOrderArrives/RandomTraderRound-triggered quote
//     decision is captured as a MarketState AT THE TRIGGER'S timestamp, then
//     only actually run (StrategyWake) at trigger_ts + latencyIn - the
//     strategy sees a state that's already stale by construction, not a
//     live one artificially delayed.
//   - latencyOut: the decided Quotes aren't posted immediately - a
//     QuoteArrives event carries them to the book at decision_ts + latencyOut.
//   - latencyCancel: cancelling a stale resting order isn't immediate either -
//     a CancelArrives event, carrying the SPECIFIC order id decided at
//     decision time (not "whatever's currently resting", which may have
//     already been replaced), removes it at decision_ts + latencyCancel.
// Because every event is ordered by timestamp regardless of kind, any
// MarketOrderArrives/QuoteArrives timestamped before a pending CancelArrives
// pops FIRST and matches normally against the still-resting stale order -
// adverse selection falls out of correct time-ordering alone, with no
// special-casing anywhere in MatchingEngine.
class SimulationEngine {
public:
    SimulationEngine(SimConfig config, Strategy& strategy);

    // Advances exactly one EVENT (see the class comment - NOT one full
    // quote-and-match round anymore). Returns false once there is genuinely
    // nothing left to do: the event queue is empty (Historical, feed
    // exhausted and every in-flight decision has resolved), maxTicks/
    // maxSimTimeNs is reached, or the kill switch has tripped.
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

    // Event handlers - each pops exactly the one event step() just took off
    // the queue and may push new events for the future, but never pops
    // another itself (that's what keeps step() at exactly one event).
    void handleMarketOrderArrives(const SimEvent& event);
    void handleStrategyWake(const SimEvent& event);
    void handleQuoteArrives(const SimEvent& event);
    void handleCancelArrives(const SimEvent& event);

    // Schedules the requoting pipeline (StrategyWake -> QuoteArrives, plus any
    // CancelArrives for what's currently resting) triggered by activity at
    // triggerTs. Shared by the Historical and Synthetic MarketOrderArrives
    // handlers - both trigger a requote the same way once the incoming
    // order/round itself has been handled.
    void scheduleRequote(int64_t triggerTs);

    // Pulls the next row from `replay` (if any) and schedules it - keeps
    // "at least one pending MarketOrderArrives, or the feed is genuinely
    // exhausted" true without ever loading the whole file as events upfront.
    void scheduleNextHistoricalOrder();

    void pushEvent(SimEvent event); // stamps event.seq and pushes - the only way anything enters `events`

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

    std::priority_queue<SimEvent, std::vector<SimEvent>, std::greater<SimEvent>> events;
    uint64_t nextEventSeq = 0;

    // Own RNG, separate from randomTrader's - drawing latency jitter from a
    // shared generator would perturb every other draw's position in that
    // generator's sequence (the exact bug Stage 1's synthetic clock hit and
    // had to be fixed the same way). Seeded off config.seed with a distinct
    // offset so it doesn't trivially correlate with randomTrader's own stream.
    std::mt19937 latencyRng;

    int64_t currentTick = 0; // now counts EVENTS processed, not rounds - see the class comment
    SimSnapshot latestSnapshot;

    // Set by the onTrade callback (constructor), read and reset once per
    // step() - see the comment there for why this needs to be reset at the
    // START of each event rather than after refreshSnapshot().
    bool tickHadFill = false;
    double tickLastFillPrice = 0.0;
    int64_t tickLastFillQty = 0;
    bool tickLastFillWasMine = false;
    OrderSide tickLastFillMineSide = OrderSide::BUY;

    // Timestamp of the most recently processed event, whatever its kind -
    // what SimSnapshot::timestampNs reports.
    int64_t lastEventTimestampNs = 0;
};
