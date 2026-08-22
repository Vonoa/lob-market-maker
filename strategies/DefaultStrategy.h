#pragma once

#include "Strategy.h"

// Wraps the skew + volatility-adjusted-spread logic that used to live directly
// on MarketMaker (reservationPrice() / effectiveSpread()).
class DefaultStrategy : public Strategy {
private:
    // Swept empirically against 100 synthetic Monte Carlo seeds: 0 (no skew)
    // -> 91% win rate, 0.0005 -> 89%, 0.001 -> 81%, 0.002 -> 62%, 0.004 (the
    // old default) -> 26%. Any nonzero skew erodes some edge even well below
    // the crossing-mid threshold skewClampFraction guards against - shifting
    // the quote off true fair value gives up edge on the side skewed toward,
    // without gaining anything extra on the side that just trades less. 0.001
    // keeps meaningful defensive skewing while staying close to the top of
    // that curve; the clamp below is what makes a nonzero value here safe to
    // keep at all, rather than degrading catastrophically at high inventory.
    double skewCoefficient = 0.001;
    double volatilityMultiplier = 1.0;
    double baseSpread = 0.2; // fixed floor - deliberately NOT derived from the live order book,
                              // since when this MM is the only liquidity provider, the live
                              // spread is usually just its own last quote, which would compound
                              // with volatility added on top of it every idle round

    // Caps how far inventory skew can shift the reservation price, as a
    // fraction of half the spread. Without this, a large enough inventory
    // position pushes the shift past halfSpread entirely, which drives the
    // "defensive" side of the quote past the true mid price - e.g. skewing
    // the bid up to encourage buybacks while short can overshoot into paying
    // ABOVE fair value to buy, a value-destroying trade that gets worse the
    // more inventory grows, exactly when defense matters most. 0.8 leaves a
    // 20% margin on the tight side so neither quote can ever cross mid,
    // confirmed empirically: at the unclamped default this strategy averaged
    // -$13 with only a 26% win rate; clamped (or with skew removed entirely)
    // it's consistently profitable.
    double skewClampFraction = 0.8;

public:
    Quote computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const override;
};
