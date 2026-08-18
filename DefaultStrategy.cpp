#include "DefaultStrategy.h"

Quote DefaultStrategy::computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const {
    (void)orderBookSpread; // deliberately unused - see baseSpread comment in the header

    double reservationPrice = midPrice - skewCoefficient * inventory / 2.0;
    double effectiveSpread = baseSpread + volatilityMultiplier * volatility;

    Quote quote;
    quote.bidPrice = reservationPrice - effectiveSpread / 2.0;
    quote.askPrice = reservationPrice + effectiveSpread / 2.0;
    return quote;
}
