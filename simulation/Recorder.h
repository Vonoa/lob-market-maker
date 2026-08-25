#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#include "SimulationEngine.h"

// Aggregate stats for a completed run - computed incrementally by Recorder as
// it observes each tick, not by re-reading the CSV afterward. One field is
// still an honest approximation rather than the roadmap's exact definition:
//   - timeWeightedAbsInventory is tick-weighted (equal weight per tick), not
//     clock-weighted
struct RunSummary {
    double  totalPnL      = 0.0;
    double  spreadPnL     = 0.0;
    double  inventoryPnL  = 0.0;
    // Annualized using each snapshot's timestampNs (mean/stdev of per-tick PnL
    // deltas, scaled by sqrt(secondsPerYear / meanSecondsPerTick)). Where
    // timestampNs isn't available (both consecutive snapshots read 0, e.g. a
    // run never wired up a real clock), falls back to the un-annualized
    // mean/stdev ratio - a stated convention, not a realism claim; see
    // Recorder::summary().
    double  sharpe        = 0.0;
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
    int64_t previousTimestampNs = 0;

    // Welford's online algorithm for the mean/variance of per-tick PnL deltas -
    // numerically stable at BTC-scale PnL (~1e9), unlike a two-pass
    // sum-of-squares-minus-mean-squared formula, which catastrophically
    // cancels at that magnitude and can produce a negative variance.
    int64_t deltaCount = 0;
    double deltaMean = 0.0;
    double deltaM2 = 0.0;
    double sumDtSeconds = 0.0; // elapsed real time between consecutive ticks, when timestamps are available

    double peakTotalPnL = 0.0;
    double maxDrawdown = 0.0;

    double sumAbsInventory = 0.0;

    double finalTotalPnL = 0.0;
    double finalSpreadPnL = 0.0;
    double finalInventoryPnL = 0.0;
    int64_t finalFillCount = 0;
};

void writeSummaryJson(const RunSummary& summary, const std::string& path);
