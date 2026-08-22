#pragma once

#include "Strategy.h"

// Deliberately simple baseline: ignores inventory and volatility entirely,
// always quotes a constant width around the raw mid.
class FixedSpreadStrategy : public Strategy {
private:
    double fixedSpread = 0.5;

public:
    Quote computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const override;
};
