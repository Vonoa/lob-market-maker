#include <chrono>
#include <iostream>
#include <vector>
#include <cmath>

#include "SimulationEngine.h"
#include "DefaultStrategy.h"
#include "FixedSpreadStrategy.h"
#include "Recorder.h"

// Replays real trade data instead of RandomTrader's synthetic flow. Note the risk
// parameters here are BTC-scale, not the ~$100 synthetic-sim defaults - see the
// scale-mismatch discussion (quantity scaling inflates exposure/PnL by 1,000,000x,
// and BTC's real price/size scale is totally different from ~$100 stocks).
//
// Recorded here, not in runSimulation()'s Monte Carlo loop - this is the one
// genuinely meaningful run worth a tick-by-tick CSV; recording all 100 Monte
// Carlo seeds would produce 100 files nobody's going to look at individually.
double runHistoricalSimulation(const std::string& filepath, int maxRows, Strategy& strategy,
                                const std::string& runLabel) {
    SimConfig config;
    config.source   = SimConfig::Source::Historical;
    config.dataPath = filepath;
    // SimulationEngine::step() now advances one EVENT, not one CSV row (see its
    // class comment) - each row can spawn up to ~5 events (the order arriving,
    // the requote decision it triggers, up to two cancels for what was
    // resting, and the new quote reaching the book). Scaling by 5 keeps
    // maxRows meaning approximately what it says rather than silently
    // stopping ~5x earlier than the caller asked for.
    config.maxTicks = maxRows * 5;
    config.quoteSize    = 2000;
    config.maxInventory = 50000;
    config.maxOrderSize = 5000;
    config.maxLoss      = 1000000000.0;
    config.maxExposure  = 5000000000.0;

    SimulationEngine engine(config, strategy);
    Recorder recorder(runLabel + "_ticks.csv");
    while (engine.step()) {
        recorder.record(engine.snapshot());
    }

    const SimSnapshot& result = engine.snapshot();

    // Real historical data trends - a totalPnL alone can't say whether that came from
    // market-making edge or just from holding a position while price drifted. Print the
    // split so it's visible right where the confound actually shows up.
    std::cout << "  spreadPnL (real $): " << result.spreadPnL / 1000000.0
               << ", inventoryPnL (real $): " << result.inventoryPnL / 1000000.0 << "\n";

    // Phase 2.3 metrics - accumulated by the recorder in the same pass as the CSV rows.
    // sharpe is now annualized from SimSnapshot's real timestampNs (see Recorder::summary());
    // timeWeightedAbsInventory is still an honest approximation (tick-weighted, not
    // clock-weighted).
    RunSummary summary = recorder.summary();
    std::cout << "  sharpe (annualized): " << summary.sharpe
               << ", maxDrawdown (real $): " << summary.maxDrawdown / 1000000.0
               << ", fillRate: " << summary.fillRate
               << ", spreadCapture/fill (real $): " << summary.spreadCapturePerFill / 1000000.0
               << ", timeWeightedAbsInventory: " << summary.timeWeightedAbsInventory << "\n";
    writeSummaryJson(summary, runLabel + "_summary.json");

    return result.totalPnL;
}

double runSimulation(unsigned int seed, Strategy& strategy) {
    SimConfig config;
    config.source   = SimConfig::Source::Synthetic;
    config.seed     = seed;
    // x5 for the events-per-row-equivalent scaling (see runHistoricalSimulation's
    // comment) - SimulationEngine::step() now advances one EVENT, not one round,
    // so this keeps roughly the old numRounds=100 worth of actual synthetic rounds.
    config.maxTicks = 100 * 5;
    config.quoteSize = 10;
    // maxInventory/maxOrderSize/maxLoss/maxExposure keep SimConfig's own defaults
    // (100/50/1000.0/5000.0), matching what MarketMaker's own defaults gave the old code.

    SimulationEngine engine(config, strategy);
    while (engine.step()) {}

    return engine.snapshot().totalPnL;
}

std::vector<double> runMonteCarlo(Strategy& strategy, int numSims) {
    std::vector<double> results;
    for (unsigned int seed = 0; seed < static_cast<unsigned int>(numSims); ++seed) {
        results.push_back(runSimulation(seed, strategy));
    }
    return results;
}

void printStats(const std::string& label, const std::vector<double>& results) {
    // Mean - same two-pass shape as FairValueModel::getVolatility(), just over
    // final PnL outcomes instead of price differences.
    double sum = 0.0;
    for (double pnl : results) {
        sum += pnl;
    }
    double mean = sum / results.size();

    // Standard deviation - second pass, using the mean from the first.
    double sumSquaredDeviations = 0.0;
    for (double pnl : results) {
        double deviation = pnl - mean;
        sumSquaredDeviations += deviation * deviation;
    }
    double variance = sumSquaredDeviations / results.size();
    double stdDev = std::sqrt(variance);

    // Win rate - fraction of runs that ended up profitable at all.
    int wins = 0;
    for (double pnl : results) {
        if (pnl > 0.0) {
            wins++;
        }
    }
    double winRate = static_cast<double>(wins) / results.size();

    std::cout << "--- " << label << " ---\n";
    std::cout << "Mean Total PnL:      " << mean << "\n";
    std::cout << "Std Dev of PnL:      " << stdDev << "\n";
    std::cout << "Win Rate:            " << (winRate * 100.0) << "%\n\n";
}

int main() {
    const int numSims = 100;

    DefaultStrategy defaultStrategy;
    FixedSpreadStrategy fixedSpreadStrategy;

    std::cout << "=== Synthetic Monte Carlo (" << numSims << " runs each) ===\n";
    printStats("DefaultStrategy", runMonteCarlo(defaultStrategy, numSims));
    printStats("FixedSpreadStrategy", runMonteCarlo(fixedSpreadStrategy, numSims));

    std::cout << "=== Historical replay (BTCUSDT, first 1000 rows) ===\n";
    double defaultHistPnL = runHistoricalSimulation(LOBSIM_DATA_DIR "/BTCUSDT-trades-2026-08-17.csv", 1000, defaultStrategy,
                                                      "run_default");
    std::cout << "DefaultStrategy     Total PnL (real $): " << defaultHistPnL / 1000000.0 << "\n";
    double fixedHistPnL = runHistoricalSimulation(LOBSIM_DATA_DIR "/BTCUSDT-trades-2026-08-17.csv", 1000, fixedSpreadStrategy,
                                                    "run_fixedspread");
    std::cout << "FixedSpreadStrategy Total PnL (real $): " << fixedHistPnL / 1000000.0 << "\n";

    // Phase 5.3 - the full file, not just the first 1000 rows. 3,000,000 rows (x5 for the
    // events-per-row scaling above) is comfortably above the file's real ~2.11M rows;
    // HistoricalDataReplay::nextOrder() returning false on genuine exhaustion is what
    // actually ends the run, not this cap.
    std::cout << "\n=== Full historical replay (BTCUSDT, entire file) ===\n";
    auto start = std::chrono::steady_clock::now();
    double fullHistPnL = runHistoricalSimulation(LOBSIM_DATA_DIR "/BTCUSDT-trades-2026-08-17.csv", 3000000, defaultStrategy,
                                                   "run_default_full");
    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();
    std::cout << "DefaultStrategy Full Total PnL (real $): " << fullHistPnL / 1000000.0 << "\n";
    std::cout << "Wall-clock: " << seconds << "s\n";

    return 0;
}
