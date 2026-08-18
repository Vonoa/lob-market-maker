#pragma once

#include "Strategy.h"

// Wraps the skew + volatility-adjusted-spread logic that used to live directly
// on MarketMaker (reservationPrice() / effectiveSpread()).
class DefaultStrategy : public Strategy {
private:
    double skewCoefficient = 0.004;
    double volatilityMultiplier = 1.0;
    double baseSpread = 0.2; // fixed floor - deliberately NOT derived from the live order book,
                              // since when this MM is the only liquidity provider, the live
                              // spread is usually just its own last quote, which would compound
                              // with volatility added on top of it every idle round

public:
    Quote computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const override;
};
