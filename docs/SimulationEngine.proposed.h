#pragma once

// PROPOSAL — Phase 2 interface, derived from the GUI mockup.
//
// Every field below exists because some panel in your design needs it. Comments name
// the panel. If you cut a panel, cut the field; if you add one, add the field here
// FIRST and the GUI stays a pure renderer with no logic of its own.
//
// Nothing in this header knows that a GUI exists. The CLI recorder writes these same
// fields to CSV. That's the point: one engine, two drivers, no duplicated logic.

#include <cstdint>
#include <string>
#include <vector>
#include <deque>

#include "Order.h"
#include "Strategy.h"

// ---------------------------------------------------------------------------
// Config — everything the top control bar can set
// ---------------------------------------------------------------------------

struct StrategyParams {
    double spreadTicks    = 4.0;   // Params panel
    int64_t quoteSize     = 5;
    int64_t inventoryLimit = 50;
    double skewCoeff      = 0.15;  // gamma
    double fairValueAlpha = 0.08;
    int    latencyTicks   = 1;     // Phase 4.5 — quote posted at t arrives at t+k
    bool   postOnly       = true;  // Phase 4.2 — never cross the touch
};

struct SimConfig {
    enum class Source { Synthetic, Historical };

    Source      source      = Source::Historical;
    std::string dataPath;                        // "Feed" dropdown
    int64_t     maxTicks    = 250000;            // "tick 41,208 / 250,000"
    unsigned    seed        = 42;                // "Seed" box
    std::string strategyName = "FixedSpread";    // "Strategy" dropdown
    StrategyParams params;
    double      maxLoss     = 1000.0;
    double      maxExposure = 5000.0;
};

// ---------------------------------------------------------------------------
// Snapshot — one frame of state. Cheap to copy; the GUI reads it every frame,
// the recorder writes it every tick.
// ---------------------------------------------------------------------------

// ORDER BOOK panel: one row of the ladder
struct BookLevel {
    double  price;
    int64_t size;         // total resting size at this price
    int     orderCount;   // the "orders" column
    int64_t myQty;        // 0 if none of it is yours — drives the "mine" column
    int64_t queueAhead;   // Phase 4.3 — size resting in front of you at this level.
                          // MISSING FROM YOUR MOCKUP. See notes; this is what turns
                          // "mine 5" into "mine 5, 42 ahead of me" and it's the
                          // difference between an optimistic sim and a credible one.
};

// TRADE TAPE panel: one printed trade
struct TapeEntry {
    int64_t   timestampMicros;
    OrderSide side;
    double    price;
    int64_t   quantity;
    bool      isMine;     // the "MINE" tag
};

// Phase 4.4 — fill quality over time. Powers the mark-out panel the mockup is missing.
struct MarkoutPoint {
    int    horizonSeconds;   // 1, 5, 30
    double avgPnLPerFill;    // signed, in ticks
    int    sampleCount;
};

struct SimSnapshot {
    // --- top bar ---
    int64_t     tick;
    int64_t     totalTicks;
    int64_t     timestampMicros;     // real tape time; 0 for synthetic
    bool        running;
    bool        killSwitchActive;
    std::string strategyName;

    // --- ORDER BOOK panel ---
    std::vector<BookLevel> bidLevels;   // best first, capped at depth (10 in the mock)
    std::vector<BookLevel> askLevels;
    double  mid;
    double  spread;
    double  imbalance;                  // (bidVol - askVol) / (bidVol + askVol)

    // --- MY QUOTES panel ---
    double  myBid, myAsk;
    int64_t myBidQty, myAskQty;
    double  quotedSpread;
    double  skewTicks;                  // signed; negative = leaning long
    int     openOrders;
    bool    atTouchBid, atTouchAsk;     // are you actually top-of-book right now?

    // --- MARKET / INVENTORY / P&L charts (scalars; GUI keeps the history) ---
    double  fairValue;
    double  volatility;
    int64_t inventory;
    int64_t inventoryLimit;

    // --- LIVE STATS panel ---
    double  cash;
    double  realisedPnL;
    double  unrealisedPnL;
    double  totalPnL;
    double  exposure;
    int64_t fillsBid, fillsAsk;
    double  fillRate;                   // fills / quotes posted
    double  avgSpreadCaptured;          // realised PnL per fill, in ticks
    double  maxDrawdown;
    double  adverseSelectionRatio;      // YOU ALREADY COMPUTE THIS AND IT'S NOT ON THE PANEL
    double  inventoryTurnsPerKTick;     // the "turns 3.4/kt" readout
    double  pctTimeAtTouch;             // MISSING — see notes
    int     killSwitchTrips;

    // --- P&L attribution. MISSING, and the most important omission. ---
    // totalPnL splits into money earned by quoting (spread capture on round trips)
    // and money made or lost by holding inventory while the market moved. A backtest
    // that reports only the total cannot tell you which one you have. On a day where
    // BTC ran 62.9k -> 64.5k, a long-biased strategy prints a great-looking number
    // that has nothing to do with market making.
    double  spreadPnL;
    double  inventoryPnL;               // spreadPnL + inventoryPnL == totalPnL

    std::vector<MarkoutPoint> markouts; // MISSING — the fill-quality panel

    // --- TRADE TAPE panel ---
    std::deque<TapeEntry> recentTrades; // ring buffer, ~50 deep

    // --- status bar ---
    double  ticksPerSecond;
    std::string recorderPath;
};

// ---------------------------------------------------------------------------
// Summary — one row per completed run. Feeds the STRATEGY COMPARISON table,
// the sweep grid, and run_<id>_summary.json.
// ---------------------------------------------------------------------------

struct RunSummary {
    SimConfig config;                   // echoed back, so a sweep is one tidy dataframe
    double  totalPnL;
    double  spreadPnL, inventoryPnL;
    double  sharpe;
    double  maxDrawdown;
    int64_t maxAbsInventory;            // the "maxInv" column
    double  fillRatePct;                // the "fill%" column
    double  adverseSelectionRatio;
    double  timeWeightedAbsInventory;
    int64_t totalFills;
    int64_t ticksRun;
    double  wallClockSeconds;
    std::vector<MarkoutPoint> markouts;
};

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

class SimulationEngine {
public:
    explicit SimulationEngine(SimConfig cfg);
    ~SimulationEngine();

    // Advances exactly one tick. Returns false when the feed is exhausted or
    // maxTicks is reached. The GUI calls this N times per frame (N = speed);
    // the CLI calls it in a while loop. Same function, both drivers.
    bool step();

    void reset();                       // "Reset" button; rewinds the feed, keeps config
    void reset(SimConfig cfg);          // changing the feed or seed

    // Live parameter edits from the Params panel. Safe mid-run — this is the
    // whole reason the GUI is worth building.
    void setParams(const StrategyParams& p);

    const SimSnapshot& snapshot() const;   // cheap; returns a reference to internal state
    const RunSummary&  summary()  const;   // valid once step() has returned false

    void setRecorder(const std::string& path);   // nullptr/"" disables
    void runToEnd();                             // headless, full speed, writes CSV

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
