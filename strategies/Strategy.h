#pragma once

#include <cstdint>
#include <limits>
#include <vector>

#include "Order.h"
#include "OrderBook.h"

// Everything a strategy can see when asked to quote. Deliberately carries both
// the per-event and per-time volatility (see FairValueModel) rather than
// picking one: DefaultStrategy/FixedSpreadStrategy read `volatility` because
// their constants were empirically tuned against that exact per-event
// quantity, while a time-driven model (Avellaneda-Stoikov) needs
// `volatilityPerSecond` instead. Reading the wrong one silently invalidates
// whatever the strategy was tuned/derived against.
struct MarketState {
    int64_t timestampNs = 0;

    // mid is the SAME reference price MarketMaker::quote() has always fed
    // strategies (referencePrice() - the live book mid, or fairValueModel's
    // trade-EWMA when useTradeBasedReference is on). microprice/fairValue are
    // additive alternatives for strategies that want them specifically.
    double mid = 0.0;
    double microprice = 0.0;
    double fairValue = 0.0;

    double bookSpread = 0.0;
    double volatility = 0.0;           // per-EVENT stdev - same meaning DefaultStrategy was tuned against
    double volatilityPerSecond = 0.0;  // per-TIME stdev - for time-driven models (e.g. Avellaneda-Stoikov)

    std::vector<BookLevel> bidLevels;
    std::vector<BookLevel> askLevels;

    int64_t inventory = 0;
    int64_t inventoryLimit = 0;

    // (T - t), normalised 1.0 -> 0.0. No session horizon is configured yet
    // (MarketMaker has no notion of one), so this is always 1.0 for now -
    // a strategy that ignores it behaves exactly as if there were no horizon.
    double timeRemaining = 1.0;
    double timeRemainingSeconds = 0.0;
};

struct QuoteSide {
    double price = 0.0;
    // Upper bound on this side's size. MarketMaker::quote() can only ever
    // SHRINK its own size decision (maxOrderSize, inventory headroom) toward
    // this value, never grow past it - defaulting to "no cap" means a
    // strategy that doesn't care about sizing needs zero extra code to get
    // today's behaviour (MarketMaker alone decides size, same as before).
    int64_t size = std::numeric_limits<int64_t>::max();
    bool active = true; // false skips this side entirely, regardless of headroom
};

struct Quotes {
    QuoteSide bid;
    QuoteSide ask;
};

// Passed to onFill() for a genuine own-fill. distanceFromMid is measured
// against the mid AT THE TIME THE QUOTE WAS POSTED (not at fill time, and not
// re-derivable from Trade alone) - the input kappa-estimation and similar
// fill-probability-by-distance models need.
struct Fill {
    OrderSide side;
    double price;
    int64_t quantity;
    double distanceFromMid;
    int64_t timestampNs;
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // NOT const: unlike the old computeQuote(), strategies can now hold and
    // update their own state between calls (kappa estimates, belief updates, ...).
    virtual Quotes computeQuotes(const MarketState& state) = 0;

    // Called for every genuine own-fill (kappa estimation, inventory-aware learning, ...).
    virtual void onFill(const Fill& fill) { (void)fill; }

    // Called for EVERY trade the matching engine executes, own or not - the
    // hook a belief-updating strategy (e.g. Glosten-Milgrom) needs to observe
    // flow it wasn't part of.
    virtual void onTrade(const Trade& trade) { (void)trade; }

    virtual void reset() {}
};
