#include "MarketMaker.h"
#include "FairValueModel.h"
#include "Logging.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>

MarketMaker::MarketMaker(OrderBook& orderBook, MatchingEngine& matchingEngine, Strategy& strategy,
                          int64_t maxInventory, int64_t maxOrderSize, double maxloss, double maxExposure)
    : orderBook(orderBook), matchingEngine(matchingEngine), strategy(strategy),
      currentBidId(NO_QUOTE_ID), currentAskId(NO_QUOTE_ID),
      maxInventory(maxInventory), maxOrderSize(maxOrderSize),
      maxloss(maxloss), maxExposure(maxExposure) {}

double MarketMaker::calcMidPrice() const {
    double bestBid = orderBook.bestBid();
    double bestAsk = orderBook.bestAsk();

    if (bestBid == 0.0 || bestAsk == 0.0) {
        // No live two-sided book right now (e.g. right after cancelQuotes() pulled our
        // own resting orders, which are often the only liquidity present). Fall back to
        // the fair value model's running estimate from past trades rather than returning
        // 0.0 - letting a fake "price of zero" flow into reservationPrice()/effectiveSpread()
        // is what produced nonsensical (even negative) quoted prices.
        return fairValueModel.getFairValue();
    }

    return (bestBid + bestAsk) / 2.0;
}

// See the header for why this exists. Same selection quote() already applied to the
// strategy's reservation price - now shared, so marks and quotes cannot disagree.
double MarketMaker::referencePrice() const {
    if (useTradeBasedReference) {
        double fairValue = fairValueModel.getFairValue();
        if (fairValue != 0.0) { // falls back to the book before the first observation
            return fairValue;
        }
    }
    return calcMidPrice();
}

Order MarketMaker::createBid(double price, int64_t quantity, int64_t timestampNs) const {
    return createOrder(OrderSide::BUY, price, quantity, timestampNs);
}

Order MarketMaker::createAsk(double price, int64_t quantity, int64_t timestampNs) const {
    return createOrder(OrderSide::SELL, price, quantity, timestampNs);
}

void MarketMaker::cancelQuotes() {
    if (currentBidId != NO_QUOTE_ID) {
        orderBook.cancelOrder(currentBidId);
        currentBidId = NO_QUOTE_ID;
        lastBidPrice = 0.0;
    }
    if (currentAskId != NO_QUOTE_ID) {
        orderBook.cancelOrder(currentAskId);
        currentAskId = NO_QUOTE_ID;
        lastAskPrice = 0.0;
    }
}

int64_t MarketMaker::getInventory() const {
    return inventory;
}

bool MarketMaker::canBuy() const {
    return inventory < maxInventory;
}

bool MarketMaker::canSell() const {
    return inventory > -maxInventory;
}

void MarketMaker::updateInventory(int64_t quantityChange, OrderSide side) {
    if (side == OrderSide::BUY) {
        inventory += quantityChange;
    } else if (side == OrderSide::SELL) {
        inventory -= quantityChange;
    }
}

double MarketMaker::getAvgCostBasis() const {
    return avgCostBasis;
}

double MarketMaker::getRealisedPnL() const {
    return realisedPnL;
}

// Must be called with the position as it stood BEFORE this fill (i.e. before updateInventory
// runs), since which of the three cases applies depends on the pre-fill sign and size.
void MarketMaker::updateCostBasis(int64_t quantity, double price, OrderSide side) {
    int dir = (side == OrderSide::BUY) ? 1 : -1;

    if (inventory == 0) {
        // Flat -> opening a fresh position: the fill price is the new cost basis.
        avgCostBasis = price;
        return;
    }

    int positionSign = (inventory > 0) ? 1 : -1;

    if (dir == positionSign) {
        // Adding to the existing position: roll the fill into a size-weighted average.
        int64_t absInventory = std::abs(inventory);
        avgCostBasis = (avgCostBasis * absInventory + price * quantity) / (absInventory + quantity);
    } else {
        // Reducing (or flipping) the position.
        int64_t closingQty = std::min(quantity, std::abs(inventory));

        // Booked here, not in onTrade(): this is the only place that knows this fill is
        // actually closing exposure (vs. opening/adding), and has the pre-fill avgCostBasis.
        realisedPnL += positionSign * (price - avgCostBasis) * closingQty;

        int64_t remainingQty = quantity - closingQty;
        if (remainingQty > 0) {
            // The fill was bigger than the open position, so it flips sides -
            // the leftover opens a brand new position at this fill's price.
            avgCostBasis = price;
        }
        // Otherwise the position shrinks but keeps its existing avgCostBasis unchanged.
    }
}
double MarketMaker::getUnrealisedPnL() const {
    if (inventory == 0) {
        return 0.0;
    }

    // referencePrice(), not calcMidPrice() - marking an open position against a stale book
    // made unrealised P&L swing on book staleness rather than on real price moves.
    double markPrice = referencePrice();
    if (markPrice == 0.0) {
        return 0.0; // no reliable price available right now - don't report a bogus swing
    }
    int positionSign = (inventory > 0) ? 1 : -1;

    return positionSign * (markPrice - avgCostBasis) * std::abs(inventory);
}
double MarketMaker::getTotalPnL() const {
    double totalPnl = realisedPnL + getUnrealisedPnL();
    // Must use the SAME price getUnrealisedPnL() marks against, or the two sides of this
    // reconciliation are computed off different prices and the warning fires constantly.
    double checkTotalPnl = cash + inventory * referencePrice() - startingCash;
    // Relative, not absolute: at BTC-scale values (~1e9-1e11 after the quantity scaling
    // and thousands of accumulated cash updates), a fixed absolute tolerance trips
    // constantly on ordinary floating-point noise. 1e-9 relative was still too tight for
    // that accumulation depth - 1e-6 stays far tighter than any real accounting bug would
    // produce while tolerating double-precision drift at this magnitude.
    double scale = std::max({1.0, std::abs(totalPnl), std::abs(checkTotalPnl)});
    if (std::abs(totalPnl - checkTotalPnl) / scale > 1e-6) {
        std::cerr << "Warning: Total PnL mismatch! TotalPnL: " << totalPnl << ", CheckTotalPnl: " << checkTotalPnl << std::endl;
    }
    return totalPnl;
}

double MarketMaker::getAdverseSelectionRatio() const {
    if (judgedFillCount == 0) {
        return 0.0;
    }
    // judgedFillCount, not totalFillCount - see the header. Only fills that have had a later
    // price observation to be judged against belong in this denominator.
    return static_cast<double>(adverseFillCount) / judgedFillCount;
}

int MarketMaker::getSelfTradeCount() const {
    return selfTradeCount;
}

int MarketMaker::getCannotQuoteCount() const {
    return cannotQuoteCount;
}

double MarketMaker::getInventoryPnL() const {
    return inventoryPnL;
}

// The residual after inventoryPnL is pulled out of totalPnL - i.e. P&L from the act of
// trading itself (buying/selling at prices better or worse than fair value), independent
// of which way the market happened to drift while inventory was held.
double MarketMaker::getSpreadPnL() const {
    return getTotalPnL() - inventoryPnL;
}

double MarketMaker::getMyBid() const {
    return lastBidPrice;
}

double MarketMaker::getMyAsk() const {
    return lastAskPrice;
}

int64_t MarketMaker::getMaxInventory() const {
    return maxInventory;
}

double MarketMaker::getCash() const {
    return cash;
}

bool MarketMaker::isKillSwitchActive() const {
    return killswitch;
}

int MarketMaker::getTotalFillCount() const {
    return totalFillCount;
}

int MarketMaker::getBuyFillCount() const {
    return buyFillCount;
}

int MarketMaker::getSellFillCount() const {
    return sellFillCount;
}

int64_t MarketMaker::getTotalFilledVolume() const {
    return totalFilledVolume;
}

double MarketMaker::getFairValue() const {
    return fairValueModel.getFairValue();
}

void MarketMaker::setUseTradeBasedReference(bool value) {
    useTradeBasedReference = value;
}

void MarketMaker::recordExternalPrice(double price, int64_t timestampNs) {
    fairValueModel.recordPrice(price, timestampNs);
}

double MarketMaker::getExposure() const {
    // referencePrice(), not calcMidPrice() - this feeds the kill switch, so valuing the
    // position off a stale book could trip it on a phantom move, or fail to on a real one.
    double markPrice = referencePrice();
    if (markPrice == 0.0) {
        return 0.0; // no reliable price available right now - don't report bogus exposure
    }
    return std::abs(inventory) * markPrice;
}

void MarketMaker::onTrade(const Trade& trade) {
    // Every trade the matching engine executes reaches here (this function IS
    // MatchingEngine's onTrade callback - see the wiring in SimulationEngine),
    // own or not, so this is the natural place to forward all of them to the
    // strategy - the hook a belief-updating strategy (e.g. Glosten-Milgrom)
    // needs to observe flow it wasn't part of. Unconditional: fires even on a
    // self-trade or a trade neither of this MM's sides was part of.
    strategy.onTrade(trade);

    // trade.price is the RESTING order's price (see MatchingEngine::process),
    // not an independent market observation - when this MM's own quote is
    // what's resting (routine at BTC scale, where quoteSize dwarfs typical
    // trade sizes and the MM dominates top-of-book), trade.price IS the MM's
    // own last quote. Feeding that back into fairValueModel is self-referential:
    // it both inflates adverse-selection judgments (the "market" the MM is
    // being graded against is partly itself) and re-closes the quote-feedback
    // loop that setUseTradeBasedReference was meant to fix. Skipped whenever
    // an external, non-self-referential feed exists instead (see
    // recordExternalPrice() and its caller in SimulationEngine::step()) -
    // Synthetic mode has no such external feed, so it still needs this.
    if (!useTradeBasedReference) {
        fairValueModel.recordPrice(trade.price, trade.timestampNs); // every fill is a real market price observation
    }

    // P&L attribution: mark-to-market the position we were ALREADY holding against this
    // trade's price, before applying this trade's own fill effects below. That ordering is
    // what keeps inventoryPnL isolated to "price moved while we held inventory" and out of
    // "we bought/sold at a price different from fair value" (which lands in spreadPnL
    // instead, via getSpreadPnL() = getTotalPnL() - inventoryPnL).
    double newMark = referencePrice();
    if (hasMarkPrice) {
        inventoryPnL += static_cast<double>(inventory) * (newMark - lastMarkPrice);
    }
    lastMarkPrice = newMark;
    hasMarkPrice = true;

    bool filledBid = trade.buyOrderId == currentBidId;
    bool filledAsk = trade.sellOrderId == currentAskId;

    // Resolve the PREVIOUS pending fill using whatever price info has arrived since -
    // including this trade's own price, already folded into fairValueModel above.
    // Only the adverse JUDGEMENT is deferred here; the fill counts themselves are taken at
    // fill time below. Counting here too used to lag every count by one fill and drop the
    // last fill of a run entirely, since nothing arrives afterwards to resolve it.
    if (hasLastFill) {
        double currentFairValue = fairValueModel.getFairValue();
        bool adverse = (lastFillSide == OrderSide::BUY && currentFairValue < lastFillPrice)
                     || (lastFillSide == OrderSide::SELL && currentFairValue > lastFillPrice);
        if (adverse) adverseFillCount++;
        judgedFillCount++;
        hasLastFill = false;
    }

    if (filledBid && filledAsk) {
        selfTradeCount++;
        if (selfTradeCount == 1) {
            // Fire-once: the first occurrence is worth seeing immediately, but a run that
            // does this repeatedly would otherwise spam stderr once per fill. The running
            // count (via getSelfTradeCount()) still shows the full extent in printStatus()/
            // the eventual run summary.
            std::cerr << "Warning: self-trade between MarketMaker's own bid (" << currentBidId
                       << ") and ask (" << currentAskId << "); inventory left unchanged. "
                       << "(further self-trades this run will be counted but not logged)" << std::endl;
        }
        return;
    }

    if (filledBid) {
        updateCostBasis(trade.quantity, trade.price, OrderSide::BUY);
        updateInventory(trade.quantity, OrderSide::BUY);
        cash -= trade.quantity * trade.price;
        lastFillPrice = trade.price;
        lastFillSide = OrderSide::BUY;
        hasLastFill = true; // judged next time a new price observation comes in
        totalFillCount++;   // counted now, not when judged - see the resolution block above
        buyFillCount++;
        totalFilledVolume += trade.quantity;
        strategy.onFill(Fill{OrderSide::BUY, trade.price, trade.quantity,
                              std::abs(trade.price - midAtLastBidQuote), trade.timestampNs});
    }
    if (filledAsk) {
        updateCostBasis(trade.quantity, trade.price, OrderSide::SELL);
        updateInventory(trade.quantity, OrderSide::SELL);
        cash += trade.quantity * trade.price;
        lastFillPrice = trade.price;
        lastFillSide = OrderSide::SELL;
        hasLastFill = true; // judged next time a new price observation comes in
        totalFillCount++;   // counted now, not when judged - see the resolution block above
        sellFillCount++;
        totalFilledVolume += trade.quantity;
        strategy.onFill(Fill{OrderSide::SELL, trade.price, trade.quantity,
                              std::abs(trade.price - midAtLastAskQuote), trade.timestampNs});
    }

    // Checked after this fill's effects are applied, so it judges PnL that actually
    // reflects the trade that just happened, not the state from before it.
    if (!killswitch && (getTotalPnL() < -maxloss || getExposure() >= maxExposure)) {
        std::cerr << "Warning: MarketMaker kill switch triggered. Total PnL: " << getTotalPnL()
                   << ", Exposure: " << getExposure() << std::endl;
        killswitch = true;
        cancelQuotes(); // stop anything still resting from getting filled further
    }
}

bool MarketMaker::checkCanQuote() {
    if (killswitch) {
        if (Logging::currentLevel >= LogLevel::Debug) {
            std::cout << "Market Maker halted - kill switch active. Call resetKillSwitch() to resume.\n";
        }
        return false;
    }

    if (calcMidPrice() == 0.0) {
        cannotQuoteCount++;
        if (cannotQuoteCount == 1) {
            // Fire-once: a stalled book means this would otherwise print every single tick
            // until data resumes, drowning out everything else. The count is still tracked
            // so a long stall is visible in printStatus()/the run summary.
            std::cerr << "Error: Cannot quote - no live market and no fair value history yet. "
                       << "(further occurrences this run will be counted but not logged)" << std::endl;
        }
        return false;
    }

    return true;
}

MarketState MarketMaker::buildMarketState(int64_t timestampNs) const {
    // Reference price for the STRATEGY's own pricing decision. Ordinarily
    // just calcMidPrice() (the live book) - useTradeBasedReference switches
    // to fairValueModel's EWMA of real trade prices instead, for the case
    // where this MM's own resting orders can dominate book liquidity (BTC
    // scale, where quoteSize dwarfs real trade sizes): calcMidPrice() would
    // otherwise mostly reflect the MM's own last quote, so re-centering the
    // NEXT quote on it compounds into a feedback loop with nothing anchoring
    // it to the real market. fairValue only moves on actual trade prints
    // (recordPrice() in onTrade(), called for every trade regardless of who
    // was on either side), so it can't be driven by the MM's own resting
    // orders sitting untouched in the book.
    double reference = referencePrice();

    MarketState state;
    state.timestampNs = timestampNs;
    state.mid = reference;
    state.microprice = orderBook.microprice();
    state.fairValue = fairValueModel.getFairValue();
    state.bookSpread = orderBook.spread();
    state.volatility = fairValueModel.getVolatility();
    state.volatilityPerSecond = fairValueModel.getVolatilityPerSecond();
    state.bidLevels = orderBook.getBidLevels(10);
    state.askLevels = orderBook.getAskLevels(10);
    state.inventory = inventory;
    state.inventoryLimit = maxInventory;
    // timeRemaining/timeRemainingSeconds left at MarketState's defaults (1.0/0.0) -
    // MarketMaker has no session-horizon concept yet.
    return state;
}

Quotes MarketMaker::decideQuotes(const MarketState& state) {
    return strategy.computeQuotes(state);
}

bool MarketMaker::hasRestingBid() const {
    return currentBidId != NO_QUOTE_ID;
}

bool MarketMaker::hasRestingAsk() const {
    return currentAskId != NO_QUOTE_ID;
}

int MarketMaker::getCurrentBidId() const {
    return currentBidId;
}

int MarketMaker::getCurrentAskId() const {
    return currentAskId;
}

void MarketMaker::cancelSpecificOrder(int orderId, OrderSide side) {
    orderBook.cancelOrder(orderId); // no-op if already gone (e.g. already filled)

    // Only clear OUR bookkeeping if this id is still what we think is resting -
    // a newer quote may already have replaced currentBidId/currentAskId by the
    // time a delayed cancel arrives, in which case this cancel targets a stale
    // order that a fresher quote has nothing to do with.
    if (side == OrderSide::BUY && currentBidId == orderId) {
        currentBidId = NO_QUOTE_ID;
        lastBidPrice = 0.0;
    } else if (side == OrderSide::SELL && currentAskId == orderId) {
        currentAskId = NO_QUOTE_ID;
        lastAskPrice = 0.0;
    }
}

// canBuy()/canSell() only decide WHETHER to quote a side. On their own they let a fill
// carry inventory past the limit by up to (size - 1), since nothing stopped a full-size
// quote going out with only a sliver of headroom left. Clamping to the remaining headroom
// is what makes maxInventory an actual limit rather than a trigger. side.size is an
// additional ceiling from the strategy itself - it can only shrink this further (defaults
// to "no cap", so a strategy that doesn't decide sizing behaves exactly as quote() always did).
bool MarketMaker::postBid(const QuoteSide& side, int64_t requestedSize, double referenceMid, int64_t timestampNs) {
    bool verbose = Logging::currentLevel >= LogLevel::Debug;
    if (!canBuy() || !side.active) {
        if (verbose) {
            std::cout << "  bid suppressed - at max inventory (" << inventory << "/" << maxInventory
                       << ") or declined by strategy\n";
        }
        return false;
    }
    int64_t clampedSize = std::min(requestedSize, maxOrderSize);
    int64_t bidSize = std::min({clampedSize, maxInventory - inventory, side.size});
    if (bidSize <= 0) {
        return false;
    }
    Order bid = createBid(side.price, bidSize, timestampNs);
    currentBidId = bid.id; // set before process() so onTrade can attribute an immediate fill
    lastBidPrice = side.price;
    midAtLastBidQuote = referenceMid;
    matchingEngine.process(bid);
    if (verbose) {
        printOrder(bid);
    }
    return true;
}

bool MarketMaker::postAsk(const QuoteSide& side, int64_t requestedSize, double referenceMid, int64_t timestampNs) {
    bool verbose = Logging::currentLevel >= LogLevel::Debug;
    if (!canSell() || !side.active) {
        if (verbose) {
            std::cout << "  ask suppressed - at min inventory (" << inventory << "/-" << maxInventory
                       << ") or declined by strategy\n";
        }
        return false;
    }
    int64_t clampedSize = std::min(requestedSize, maxOrderSize);
    int64_t askSize = std::min({clampedSize, maxInventory + inventory, side.size}); // headroom on the short side
    if (askSize <= 0) {
        return false;
    }
    Order ask = createAsk(side.price, askSize, timestampNs);
    currentAskId = ask.id; // set before process() so onTrade can attribute an immediate fill
    lastAskPrice = side.price;
    midAtLastAskQuote = referenceMid;
    matchingEngine.process(ask);
    if (verbose) {
        printOrder(ask);
    }
    return true;
}

void MarketMaker::quote(int64_t quantity, int64_t timestampNs) {
    if (!checkCanQuote()) {
        return;
    }

    // Built once, up front, so both sides price off the exact same snapshot of
    // market state rather than risking it changing between two separate calls.
    MarketState state = buildMarketState(timestampNs);
    Quotes thisQuote = decideQuotes(state);

    cancelQuotes();

    if (Logging::currentLevel >= LogLevel::Debug) {
        std::cout << "Market Maker Quotes:\n";
    }

    // Route through the matching engine (not orderBook.addOrder directly) so a quote
    // that crosses the touch - e.g. once skew shifts it off center - actually executes
    // instead of resting uncrossed in the book.
    postBid(thisQuote.bid, quantity, state.mid, timestampNs);
    postAsk(thisQuote.ask, quantity, state.mid, timestampNs);
}
void MarketMaker::resetKillSwitch() {
    killswitch = false;
}

void MarketMaker::printStatus() const {
    std::cout << "Market Maker Status:\n";
    std::cout << "  Inventory: " << inventory << "\n";
    std::cout << "  Avg Cost Basis: " << avgCostBasis << "\n";
    std::cout << "  Realised PnL: " << realisedPnL << "\n";
    std::cout << "  Unrealised PnL: " << getUnrealisedPnL() << "\n";
    std::cout << "  Total PnL: " << getTotalPnL() << "\n";
    std::cout << "  Spread PnL: " << getSpreadPnL() << "\n";
    std::cout << "  Inventory PnL: " << getInventoryPnL() << "\n";
    std::cout << "  Exposure: " << getExposure() << "\n";
    std::cout << "  Adverse Selection Ratio: " << getAdverseSelectionRatio()
               << " (" << adverseFillCount << "/" << judgedFillCount << " judged, "
               << totalFillCount << " filled)\n";
    std::cout << "  Self-Trades: " << selfTradeCount << "\n";
    std::cout << "  Cannot-Quote Occurrences: " << cannotQuoteCount << "\n";
}