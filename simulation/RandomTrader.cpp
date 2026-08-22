#include "RandomTrader.h"

RandomTrader::RandomTrader(OrderBook& orderBook, unsigned int seed)
    : orderBook(orderBook), rng(seed) {}

void RandomTrader::maybeSwitchRegime() {
    roundsSinceSwitch++;
    if (roundsSinceSwitch >= regimeDuration) {
        if (currentRegime == Regime::Calm) {
            currentRegime = Regime::Volatile;
        } else if (currentRegime == Regime::Volatile) {
            currentRegime = Regime::Trending;
        } else {
            currentRegime = Regime::Calm;
        }
        roundsSinceSwitch = 0;
    }
}

void RandomTrader::updateRegimeParameters() {
    switch (currentRegime) {
        case Regime::Calm:
            buyProbability = 0.5;
            effectivePriceRange = priceRange;
            tradeProbability = 0.4; // quiet - trades happen less often
            break;
        case Regime::Volatile:
            buyProbability = 0.5;
            effectivePriceRange = priceRange * 3;
            tradeProbability = 0.9; // choppy - trades happen almost every round
            break;
        case Regime::Trending:
            buyProbability = 0.7;
            effectivePriceRange = priceRange;
            tradeProbability = 0.7; // steady directional activity
            break;
    }
}

RandomTrader::Regime RandomTrader::getCurrentRegime() const {
    return currentRegime;
}

bool RandomTrader::shouldTrade() {
    // Advances regime state exactly once per round - generateOrder() relies on this
    // having already run this round and does not call these itself.
    maybeSwitchRegime();
    updateRegimeParameters();

    std::bernoulli_distribution tradeRoll(tradeProbability);
    return tradeRoll(rng);
}

Order RandomTrader::generateOrder() {
    double bestBid = orderBook.bestBid();
    double bestAsk = orderBook.bestAsk();

    std::bernoulli_distribution coinFlip(buyProbability);
    OrderSide side = coinFlip(rng) ? OrderSide::BUY : OrderSide::SELL;

    // Always price past the current touch, so this order matches immediately as a taker
    // instead of resting - resting orders never expire, so they'd otherwise pile up
    // unboundedly and let the book drift as a pure artifact of the simulation, not the market.
    std::uniform_real_distribution<double> priceBuffer(0.0, effectivePriceRange);
    double price = (side == OrderSide::BUY)
        ? bestAsk + priceBuffer(rng)
        : bestBid - priceBuffer(rng);

    // Guard against a one-sided/empty book (bestBid's 0.0 "no data" sentinel) or a large
    // random offset driving the price non-positive - clamp to a small positive floor.
    if (price <= 0.0) {
        price = 0.01;
    }

    std::uniform_int_distribution<int> sizeDist(minSize, maxSize);
    int quantity = sizeDist(rng);

    return createOrder(side, price, quantity);
}
