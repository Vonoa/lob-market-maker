#include "doctest/doctest.h"
#include "SimulationEngine.h"
#include "DefaultStrategy.h"
#include "SimEvent.h"

#include <random>
#include <string>

// SimEvent's ordering is what makes the whole latency mechanism work (an
// earlier-timestamped event must always pop before a later one, regardless of
// which handler scheduled it or when) - cheap and worth pinning down directly,
// without needing a full SimulationEngine to exercise it.
TEST_CASE("SimEvent orders earlier timestamps first, then insertion order for ties") {
    SimEvent earlier;
    earlier.ts = 100;
    earlier.seq = 5;
    SimEvent later;
    later.ts = 200;
    later.seq = 0;
    CHECK_FALSE(earlier > later); // an earlier timestamp must never be "greater", even with a higher seq
    CHECK(later > earlier);

    SimEvent firstInserted;
    firstInserted.ts = 100;
    firstInserted.seq = 0;
    SimEvent secondInserted;
    secondInserted.ts = 100;
    secondInserted.seq = 1;
    CHECK_FALSE(firstInserted > secondInserted); // same ts - lower seq (inserted first) sorts first
    CHECK(secondInserted > firstInserted);
}

TEST_CASE("LatencyModel: zero jitter returns a constant delay") {
    LatencyModel model{100.0, 0.0};
    std::mt19937 rng(1);
    CHECK(model.sample(rng) == 100);
    CHECK(model.sample(rng) == 100); // constant regardless of how many times it's drawn
}

TEST_CASE("LatencyModel: same seed produces the same jitter sequence") {
    LatencyModel model{100.0, 50.0};
    std::mt19937 rngA(42);
    std::mt19937 rngB(42);
    for (int i = 0; i < 5; ++i) {
        CHECK(model.sample(rngA) == model.sample(rngB));
    }
}

TEST_CASE("Event-driven SimulationEngine is deterministic for a fixed seed") {
    SimConfig config;
    config.source = SimConfig::Source::Synthetic;
    config.seed = 7;
    config.maxTicks = 500;
    config.quoteSize = 10;
    // Nonzero on all three - determinism has to hold with latencyRng draws in
    // play too, not just in the (trivially deterministic) zero-latency case.
    config.latencyIn = LatencyModel{50000.0, 20000.0};
    config.latencyOut = LatencyModel{30000.0, 10000.0};
    config.latencyCancel = LatencyModel{40000.0, 15000.0};

    DefaultStrategy strategyA;
    SimulationEngine engineA(config, strategyA);
    while (engineA.step()) {}

    DefaultStrategy strategyB;
    SimulationEngine engineB(config, strategyB);
    while (engineB.step()) {}

    const SimSnapshot& a = engineA.snapshot();
    const SimSnapshot& b = engineB.snapshot();

    CHECK(a.tick == b.tick);
    CHECK(a.timestampNs == b.timestampNs);
    CHECK(a.inventory == b.inventory);
    CHECK(a.fillCount == b.fillCount);
    CHECK(a.totalPnL == doctest::Approx(b.totalPnL));
}

// Same fixed external order flow (a real CSV - Historical mode doesn't read
// its own book to decide what arrives, unlike Synthetic's RandomTrader), only
// latencyCancel differs. If the outcome is identical either way, the cancel
// path isn't actually doing anything.
TEST_CASE("Nonzero latencyCancel changes historical replay outcomes") {
    std::string dataFile = std::string(LOBSIM_DATA_DIR) + "/BTCUSDT-trades-2026-08-17.csv";

    SimConfig zeroLatencyConfig;
    zeroLatencyConfig.source = SimConfig::Source::Historical;
    zeroLatencyConfig.dataPath = dataFile;
    zeroLatencyConfig.maxTicks = 2000; // a few hundred rows' worth of events
    zeroLatencyConfig.quoteSize = 2000;
    zeroLatencyConfig.maxInventory = 50000;
    zeroLatencyConfig.maxOrderSize = 5000;
    zeroLatencyConfig.maxLoss = 1e9;
    zeroLatencyConfig.maxExposure = 5e9;

    SimConfig laggedConfig = zeroLatencyConfig;
    // 5ms fixed cancel delay - large relative to typical BTCUSDT inter-trade
    // gaps, so a stale quote should still be resting for at least some of the
    // trades that arrive while its cancel is in flight.
    laggedConfig.latencyCancel = LatencyModel{5'000'000.0, 0.0};

    DefaultStrategy strategyZero;
    SimulationEngine zeroEngine(zeroLatencyConfig, strategyZero);
    while (zeroEngine.step()) {}

    DefaultStrategy strategyLagged;
    SimulationEngine laggedEngine(laggedConfig, strategyLagged);
    while (laggedEngine.step()) {}

    const SimSnapshot& zero = zeroEngine.snapshot();
    const SimSnapshot& lagged = laggedEngine.snapshot();

    bool outcomesDiffer = zero.fillCount != lagged.fillCount
                        || zero.totalPnL != doctest::Approx(lagged.totalPnL)
                        || zero.inventory != lagged.inventory;
    CHECK(outcomesDiffer);
}
