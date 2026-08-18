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

    void maybeSwitchRegime();
    void updateRegimeParameters();

public:
    explicit RandomTrader(OrderBook& orderBook, unsigned int seed = 42);

    Order generateOrder();
    bool shouldTrade();
    Regime getCurrentRegime() const;
};
