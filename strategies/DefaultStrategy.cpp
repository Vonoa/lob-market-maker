#include "DefaultStrategy.h"
#include <algorithm>

Quotes DefaultStrategy::computeQuotes(const MarketState& state) {
    // state.bookSpread deliberately unused - see baseSpread comment in the header.
    // state.volatility (per-EVENT), not volatilityPerSecond - this is the exact
    // quantity these constants were empirically tuned against.
    double effectiveSpread = baseSpread + volatilityMultiplier * state.volatility;
    double halfSpread = effectiveSpread / 2.0;

    // Clamped to skewClampFraction * halfSpread - see the header comment for
    // why an unclamped shift can push a quote past the true mid price.
    double rawShift = skewCoefficient * state.inventory / 2.0;
    double maxShift = skewClampFraction * halfSpread;
    double shift = std::max(-maxShift, std::min(maxShift, rawShift));

    double reservationPrice = state.mid - shift;

    Quotes quotes;
    quotes.bid.price = reservationPrice - halfSpread;
    quotes.ask.price = reservationPrice + halfSpread;
    return quotes;
}
