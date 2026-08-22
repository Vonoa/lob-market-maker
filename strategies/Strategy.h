#pragma once

#include <cstdint>

struct Quote {
    double bidPrice;
    double askPrice;
};

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual Quote computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const = 0;
};
