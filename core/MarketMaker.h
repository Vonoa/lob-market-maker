#pragma once

#include "Order.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "FairValueModel.h"
#include "Strategy.h"

class MarketMaker {
private:
    OrderBook& orderBook;
    MatchingEngine& matchingEngine;
    FairValueModel fairValueModel;
    Strategy& strategy;

    static constexpr int NO_QUOTE_ID = -1;
    int currentBidId;
    int currentAskId;
    int64_t inventory = 0;
    int64_t maxInventory;
    int64_t maxOrderSize;
    double startingCash = 100000.0; // Starting cash for the market maker
    double cash = startingCash; // Current cash balance for the market maker
    double avgCostBasis = 0.0; // Weighted-average price of the current open position
    double realisedPnL = 0.0; // Realized profit and loss from closed positions
    double unrealisedPnL = 0.0; // Unrealized profit and loss from open positions
    bool killswitch = false; // If true, the market maker will not quote new orders
    double maxloss;
    double maxExposure;
    double lastFillPrice = 0.0;
    OrderSide lastFillSide = OrderSide::BUY;
    bool hasLastFill = false;
    int adverseFillCount = 0;
    int totalFillCount = 0; // counted at fill time, so the final fill of a run is included
    int buyFillCount = 0;   // subset of totalFillCount, same fill-time counting
    int sellFillCount = 0;
    // Denominator for the adverse-selection ratio, NOT interchangeable with totalFillCount.
    // A fill can only be judged adverse once a later price observation exists to judge it
    // against, so judgement necessarily lags counting by one fill (and the last fill of a
    // run is never judged at all). Dividing adverseFillCount by totalFillCount would mix a
    // judged numerator with an unjudged denominator and quietly understate the ratio.
    int judgedFillCount = 0;
    int64_t totalFilledVolume = 0; // cumulative filled quantity (both sides), for turnover readouts
    int selfTradeCount = 0;
    int cannotQuoteCount = 0;
    double inventoryPnL = 0.0; // mark-to-market P&L from holding inventory while price moved
    double lastMarkPrice = 0.0;
    bool hasMarkPrice = false;
    double lastBidPrice = 0.0; // price of the currently-resting bid, if any (0.0 if none)
    double lastAskPrice = 0.0; // price of the currently-resting ask, if any (0.0 if none)

    // The reference price EACH side was quoted against, captured at quote() time -
    // Strategy::onFill()'s Fill::distanceFromMid needs this, and it isn't
    // recoverable from Trade alone (trade.price is the RESTING order's price,
    // and the mid may have moved between posting the quote and it filling).
    double midAtLastBidQuote = 0.0;
    double midAtLastAskQuote = 0.0;

    // Off by default - calcMidPrice() (the live book) is a perfectly good
    // reference price when this MM's own orders are a small fraction of
    // book liquidity (true at the synthetic ~$100 scale, where quoteSize is
    // comparable to real order sizes). Only worth turning on where the MM's
    // own resting orders can dominate the book - see setUseTradeBasedReference().
    bool useTradeBasedReference = false;

public:
    MarketMaker(OrderBook& orderBook, MatchingEngine& matchingEngine, Strategy& strategy,
                int64_t maxInventory = 100, int64_t maxOrderSize = 50,
                double maxloss = 1000.0, double maxExposure = 5000.0);

    Order createBid(double price, int64_t quantity, int64_t timestampNs = 0) const;
    Order createAsk(double price, int64_t quantity, int64_t timestampNs = 0) const;

    double calcMidPrice() const;

    // The price this MM values itself against: fairValueModel's external-trade EWMA when
    // useTradeBasedReference is on, otherwise the live book's mid.
    //
    // Exists because calcMidPrice() alone is not trustworthy during historical replay -
    // MatchingEngine rests every unfilled remainder and nothing ever cancels a replay order,
    // so the book accumulates stale liquidity a real exchange would have pulled. bestBid()/
    // bestAsk() then lag the real market for long stretches and catch up in jumps.
    //
    // quote() already made this choice for the STRATEGY's reservation price. Everything that
    // marks a position has to make the same choice, or marks and quotes disagree: a step-jump
    // in a stale mid injects (inventory x jump) into inventoryPnL, and the equal-and-opposite
    // error into getSpreadPnL(), which is defined as that residual. That made the spread-vs-
    // inventory P&L split read as a directional strategy when it was measuring book staleness.
    double referencePrice() const;

    // timestampNs is threaded through explicitly (not stored via a setter) so
    // MarketMaker stays synchronously testable - see tests/test_cost_basis.cpp,
    // which calls quote() directly without a SimulationEngine driving a clock.
    // Implemented in terms of the decomposed primitives below (decideQuotes()/
    // postBid()/postAsk()) - kept as the single synchronous decide-cancel-post
    // call for direct/test use. An event-driven caller wanting latency between
    // those steps (see SimulationEngine) calls the primitives itself instead.
    void quote(int64_t quantity, int64_t timestampNs = 0);
    void cancelQuotes();

    // --- Decomposed quoting primitives -------------------------------------
    // MarketMaker itself has no notion of time or delay - it stays "timing-
    // agnostic" and exposes intent (decide, cancel this specific order, post
    // that specific side) so a caller like SimulationEngine can space these
    // calls out over simulated time to model latency, without MarketMaker
    // needing to know latency exists.

    // Same guard quote() has always applied (killswitch halt / no live market
    // yet), including its exact messaging and cannotQuoteCount bookkeeping -
    // pulled out so an event-driven caller (SimulationEngine) gets identical
    // behaviour to the synchronous quote() path instead of a re-derived copy.
    // Returns true iff it's OK to proceed with buildMarketState()/decideQuotes().
    bool checkCanQuote();

    // Snapshots current book/inventory/fair-value state as of timestampNs.
    // Building this now vs. later matters: a caller modelling "the strategy
    // sees a stale view" (latencyIn) must capture this AT THE EARLIER TIME and
    // hand the SAME frozen state to decideQuotes() later - rebuilding it at
    // decision time would read a MORE current book, the opposite of stale.
    MarketState buildMarketState(int64_t timestampNs) const;

    // Runs the strategy against a (possibly stale/frozen) MarketState. Not
    // const: strategies may hold and update their own state.
    Quotes decideQuotes(const MarketState& state);

    bool hasRestingBid() const;
    bool hasRestingAsk() const;
    int getCurrentBidId() const; // meaningful only if hasRestingBid()
    int getCurrentAskId() const; // meaningful only if hasRestingAsk()

    // Cancels a SPECIFIC previously-posted order by id, not "whatever is
    // currently resting" - needed because a newer quote may already have
    // replaced currentBidId/currentAskId by the time a delayed cancel
    // actually arrives. Only clears this MM's own bookkeeping
    // (currentBidId/lastBidPrice etc.) if that id is still the current one;
    // otherwise just removes the stale order from the book and leaves the
    // newer quote's bookkeeping alone. Safe to call on an id already gone
    // from the book (OrderBook::cancelOrder() is a no-op then).
    void cancelSpecificOrder(int orderId, OrderSide side);

    // Posts one side against a strategy-decided QuoteSide, applying the same
    // maxOrderSize/inventory-headroom/strategy-size-ceiling clamping quote()
    // always has. requestedSize is the caller's raw configured quote size
    // (pre-maxOrderSize-clamp, matching quote()'s `quantity` parameter).
    // referenceMid is the mid AT DECISION TIME (state.mid from the
    // MarketState decideQuotes() was called with) - recorded as
    // midAtLastBidQuote/midAtLastAskQuote for Fill::distanceFromMid, even
    // though the order is only actually posted now (possibly later, under
    // latencyOut). Returns false (no-op) if inventory/strategy declined this
    // side or the clamped size came out non-positive.
    bool postBid(const QuoteSide& side, int64_t requestedSize, double referenceMid, int64_t timestampNs);
    bool postAsk(const QuoteSide& side, int64_t requestedSize, double referenceMid, int64_t timestampNs);
    int64_t getInventory() const;
    bool canBuy() const;
    bool canSell() const;
    void updateInventory(int64_t quantityChange, OrderSide side);
    void updateCostBasis(int64_t quantity, double price, OrderSide side);
    double getAvgCostBasis() const;
    double getRealisedPnL() const;
    void onTrade(const Trade& trade);
    double getUnrealisedPnL() const;
    double getTotalPnL() const;
    void printStatus() const;
    void resetKillSwitch();
    double getExposure() const;
    double getAdverseSelectionRatio() const;
    int getSelfTradeCount() const;
    int getCannotQuoteCount() const;
    double getInventoryPnL() const;
    double getSpreadPnL() const;
    double getMyBid() const; // 0.0 if no bid currently resting
    double getMyAsk() const; // 0.0 if no ask currently resting
    int64_t getMaxInventory() const;
    double getCash() const;
    bool isKillSwitchActive() const;
    int getTotalFillCount() const;
    int getBuyFillCount() const;
    int getSellFillCount() const;
    int64_t getTotalFilledVolume() const;
    double getFairValue() const;

    // When true, quote() prices the strategy's reservation price off
    // fairValueModel (real trade prices only) instead of calcMidPrice() (the
    // live book) - fixes the feedback loop where quoting large enough orders
    // to dominate book liquidity means calcMidPrice() is mostly reading the
    // MM's own last quote back to itself. Meant for historical/BTC-scale
    // replay specifically, not a universal improvement - see the caller.
    void setUseTradeBasedReference(bool value);

    // Feeds fairValueModel from a price NOT derived from this sim's own
    // matching (e.g. the raw historical exchange price of an incoming order,
    // before it's matched against anything). See the caller in
    // SimulationEngine::step() and the comment on onTrade()'s own
    // recordPrice() call for why this exists.
    void recordExternalPrice(double price, int64_t timestampNs = 0);
};
