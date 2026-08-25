#pragma once

#include <random>
#include "Order.h"
#include "OrderBook.h"

class RandomTrader {
public:
    enum class Regime { Calm, Volatile, Trending };

private:
    OrderBook& orderBook;
    std::mt19937 rng;
    double priceRange = 0.5; // max distance a random order's price can land from the current mid
    int minSize = 1;
    int maxSize = 10;

    Regime currentRegime = Regime::Calm;
    int roundsSinceSwitch = 0;
    int regimeDuration = 20; // rounds before cycling to the next regime

    // Set together by updateRegimeParameters() based on currentRegime, so generateOrder()
    // and shouldTrade() both read the same regime-appropriate values.
    double buyProbability = 0.5;
    double effectivePriceRange = 0.5;
    double tradeProbability = 0.5;

    // Synthetic wall clock, advanced once per round in shouldTrade() (same "once per
    // round" contract maybeSwitchRegime() already relies on) so Synthetic-mode orders
    // get a real, monotonically increasing timestampNs instead of the createOrder()
    // default of 0 - needed for anything downstream that measures elapsed time
    // (volatility per second, latency, markouts).
    //
    // Draws from its OWN rng (clockRng), not the shared `rng` used for trade/side/
    // price/size decisions - drawing from the same generator would shift every
    // downstream draw's position in the sequence and change this strategy's
    // empirically-tuned Monte Carlo behaviour purely as a side effect of adding a
    // clock (confirmed: it moved DefaultStrategy's win rate from 81% to 78%).
    int64_t simClockNs = 0;
    double meanInterArrivalNs = 100'000'000.0; // 100ms mean gap between rounds (~10/sec)
    std::mt19937 clockRng;
    std::exponential_distribution<double> interArrivalUnit{1.0}; // scaled by meanInterArrivalNs below

    void maybeSwitchRegime();
    void updateRegimeParameters();

public:
    explicit RandomTrader(OrderBook& orderBook, unsigned int seed = 42);

    Order generateOrder();
    bool shouldTrade();
    Regime getCurrentRegime() const;
    int64_t currentTimeNs() const;
};
