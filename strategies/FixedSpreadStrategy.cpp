#include "FixedSpreadStrategy.h"
Quotes FixedSpreadStrategy::computeQuotes(const MarketState& state) {
    Quotes quotes;
    quotes.bid.price = state.mid - fixedSpread / 2.0;
    quotes.ask.price = state.mid + fixedSpread / 2.0;
    return quotes;
}