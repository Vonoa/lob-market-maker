#include "FixedSpreadStrategy.h"
Quote FixedSpreadStrategy::computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const {
    Quote quote;
    quote.bidPrice = midPrice - fixedSpread / 2.0;
    quote.askPrice = midPrice + fixedSpread /2.0;
    return quote;
}