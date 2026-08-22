#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "SimulationEngine.h"

// Aggregate stats for a completed run - computed incrementally by Recorder as
// it observes each tick, not by re-reading the CSV afterward. Two fields are
// honest approximations rather than the roadmap's exact definitions, because
// SimSnapshot doesn't carry real tape timestamps or a tick-size yet:
//   - sharpe is un-annualized (mean/stdev of per-tick PnL deltas, not scaled
//     by steps-per-day)
//   - timeWeightedAbsInventory is tick-weighted (equal weight per tick), not
//     clock-weighted
struct RunSummary {
    double  totalPnL      = 0.0;
    double  spreadPnL     = 0.0;
    double  inventoryPnL  = 0.0;
    double  sharpe        = 0.0; // un-annualized - see note above
    double  maxDrawdown   = 0.0;
    double  fillRate      = 0.0; // fillCount / ticksRun
    double  spreadCapturePerFill = 0.0; // spreadPnL / fillCount, raw price units (no tick size yet)
    double  timeWeightedAbsInventory = 0.0; // tick-weighted - see note above
    int64_t fillCount = 0;
    int64_t ticksRun  = 0;
};

// Writes one CSV row per tick from a SimulationEngine run, and accumulates the
// running statistics RunSummary needs in the same single pass - driven from
// outside, same shape as: while (engine.step()) recorder.record(engine.snapshot());
class Recorder {
public:
    explicit Recorder(const std::string& path);

    void record(const SimSnapshot& snapshot);

    // Valid any time after at least one record() call; reflects everything
    // observed so far (doesn't require the run to have finished).
    RunSummary summary() const;

private:
    std::ofstream file;

    int64_t tickCount = 0;

    double previousTotalPnL = 0.0;
    double sumPnLDelta = 0.0;
    double sumPnLDeltaSquared = 0.0;

    double peakTotalPnL = 0.0;
    double maxDrawdown = 0.0;

    double sumAbsInventory = 0.0;

    double finalTotalPnL = 0.0;
    double finalSpreadPnL = 0.0;
    double finalInventoryPnL = 0.0;
    int64_t finalFillCount = 0;
};

void writeSummaryJson(const RunSummary& summary, const std::string& path);
