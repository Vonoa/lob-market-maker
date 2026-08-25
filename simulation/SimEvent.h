#pragma once

#include <cstdint>

#include "Order.h"
#include "Strategy.h"

// One entry in SimulationEngine's timestamp-ordered event queue. A single
// struct with a Kind tag (not a class hierarchy) - the payload is small and
// the four kinds are fixed, so a hierarchy would be pure ceremony here.
struct SimEvent {
    enum class Kind {
        MarketOrderArrives, // Historical: a parsed CSV row. Synthetic: one RandomTrader round.
        StrategyWake,       // The MM's decision fires now, against the FROZEN state carried below.
        QuoteArrives,       // A previously-decided quote actually reaches the book now.
        CancelArrives       // A previously-decided cancel actually reaches the book now.
    };

    int64_t ts = 0;
    Kind kind = Kind::MarketOrderArrives;
    uint64_t seq = 0; // deterministic tie-break for equal timestamps (FIFO within the same ts)

    // --- payload - only the field(s) matching `kind` are meaningful ---

    Order order;                 // MarketOrderArrives (Historical: pre-parsed; Synthetic: pre-built if willTrade)
    bool willTrade = false;      // MarketOrderArrives (Synthetic only) - whether `order` is populated

    MarketState marketState;     // StrategyWake - captured at the ORIGINAL trigger time, not this event's ts,
                                  // so a nonzero latencyIn makes the strategy decide against a genuinely stale view

    Quotes decidedQuotes;        // QuoteArrives
    double decisionMid = 0.0;    // QuoteArrives - mid at DECISION time, for Fill::distanceFromMid

    int cancelOrderId = 0;       // CancelArrives
    OrderSide cancelSide = OrderSide::BUY; // CancelArrives

    // Min-heap by (ts, seq) - std::priority_queue is a max-heap by default,
    // so `greater` here (combined with std::greater<> as the queue's Compare)
    // is what makes the earliest timestamp come out first.
    bool operator>(const SimEvent& other) const {
        return ts != other.ts ? ts > other.ts : seq > other.seq;
    }
};
