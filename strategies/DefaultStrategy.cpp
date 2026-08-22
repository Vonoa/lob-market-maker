#include "DefaultStrategy.h"
#include <algorithm>

Quote DefaultStrategy::computeQuote(double midPrice, int64_t inventory, double volatility, double orderBookSpread) const {
    (void)orderBookSpread; // deliberately unused - see baseSpread comment in the header

    double effectiveSpread = baseSpread + volatilityMultiplier * volatility;
    double halfSpread = effectiveSpread / 2.0;

    // Clamped to skewClampFraction * halfSpread - see the header comment for
    // why an unclamped shift can push a quote past the true mid price.
    double rawShift = skewCoefficient * inventory / 2.0;
    double maxShift = skewClampFraction * halfSpread;
    double shift = std::max(-maxShift, std::min(maxShift, rawShift));

    double reservationPrice = midPrice - shift;

    Quote quote;
    quote.bidPrice = reservationPrice - halfSpread;
    quote.askPrice = reservationPrice + halfSpread;
    return quote;
}
