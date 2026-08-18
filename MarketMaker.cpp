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

Order MarketMaker::createBid(double price, int64_t quantity) const {
    return createOrder(OrderSide::BUY, price, quantity);
}

Order MarketMaker::createAsk(double price, int64_t quantity) const {
    return createOrder(OrderSide::SELL, price, quantity);
}

void MarketMaker::cancelQuotes() {
    if (currentBidId != NO_QUOTE_ID) {
        orderBook.cancelOrder(currentBidId);
        currentBidId = NO_QUOTE_ID;
    }
    if (currentAskId != NO_QUOTE_ID) {
        orderBook.cancelOrder(currentAskId);
        currentAskId = NO_QUOTE_ID;
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

    double midPrice = calcMidPrice();
    if (midPrice == 0.0) {
        return 0.0; // no reliable price available right now - don't report a bogus swing
    }
    int positionSign = (inventory > 0) ? 1 : -1;

    return positionSign * (midPrice - avgCostBasis) * std::abs(inventory);
}
double MarketMaker::getTotalPnL() const {
    double totalPnl = realisedPnL + getUnrealisedPnL();
    double checkTotalPnl = cash + inventory * calcMidPrice() - startingCash;
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
    if (totalFillCount == 0) {
        return 0.0;
    }
    return static_cast<double>(adverseFillCount) / totalFillCount;
}

double MarketMaker::getExposure() const {
    double midPrice = calcMidPrice();
    if (midPrice == 0.0) {
        return 0.0; // no reliable price available right now - don't report bogus exposure
    }
    return std::abs(inventory) * midPrice;
}

void MarketMaker::onTrade(const Trade& trade) {
    fairValueModel.recordPrice(trade.price); // every fill is a real market price observation

    bool filledBid = trade.buyOrderId == currentBidId;
    bool filledAsk = trade.sellOrderId == currentAskId;

    // Resolve the PREVIOUS pending fill using whatever price info has arrived since -
    // including this trade's own price, already folded into fairValueModel above.
    if (hasLastFill) {
        double currentFairValue = fairValueModel.getFairValue();
        bool adverse = (lastFillSide == OrderSide::BUY && currentFairValue < lastFillPrice)
                     || (lastFillSide == OrderSide::SELL && currentFairValue > lastFillPrice);
        if (adverse) adverseFillCount++;
        totalFillCount++;
        hasLastFill = false;
    }

    if (filledBid && filledAsk) {
        std::cerr << "Warning: self-trade between MarketMaker's own bid (" << currentBidId
                   << ") and ask (" << currentAskId << "); inventory left unchanged." << std::endl;
        return;
    }

    if (filledBid) {
        updateCostBasis(trade.quantity, trade.price, OrderSide::BUY);
        updateInventory(trade.quantity, OrderSide::BUY);
        cash -= trade.quantity * trade.price;
        lastFillPrice = trade.price;
        lastFillSide = OrderSide::BUY;
        hasLastFill = true; // judged next time a new price observation comes in
    }
    if (filledAsk) {
        updateCostBasis(trade.quantity, trade.price, OrderSide::SELL);
        updateInventory(trade.quantity, OrderSide::SELL);
        cash += trade.quantity * trade.price;
        lastFillPrice = trade.price;
        lastFillSide = OrderSide::SELL;
        hasLastFill = true; // judged next time a new price observation comes in
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

void MarketMaker::quote(int64_t quantity) {
    if (killswitch) {
        if (Logging::currentLevel >= LogLevel::Debug) {
            std::cout << "Market Maker halted - kill switch active. Call resetKillSwitch() to resume.\n";
        }
        return;
    }

    // Checks calcMidPrice() itself (live book, or the fair-value fallback), not the raw
    // book directly - the raw book alone would still fail this the instant cancelQuotes()
    // below pulls our own resting orders, even though we can now still price off fair value.
    if (calcMidPrice() == 0.0) {
        std::cerr << "Error: Cannot quote - no live market and no fair value history yet." << std::endl;
        return;
    }
    int64_t size = std::min(quantity, maxOrderSize); // clamp to the max single-order size rather than refusing to quote at all

    // Computed once, up front, so both sides price off the exact same snapshot of
    // market state rather than risking it changing between two separate calls.
    Quote thisQuote = strategy.computeQuote(calcMidPrice(), inventory, fairValueModel.getVolatility(), orderBook.spread());

    cancelQuotes();

    bool verbose = Logging::currentLevel >= LogLevel::Debug;
    if (verbose) {
        std::cout << "Market Maker Quotes:\n";
    }

    // Route through the matching engine (not orderBook.addOrder directly) so a quote
    // that crosses the touch - e.g. once skew shifts it off center - actually executes
    // instead of resting uncrossed in the book.
    if (canBuy()) {
        Order bid = createBid(thisQuote.bidPrice, size);
        currentBidId = bid.id; // set before process() so onTrade can attribute an immediate fill
        matchingEngine.process(bid);
        if (verbose) {
            printOrder(bid);
        }
    } else if (verbose) {
        std::cout << "  bid suppressed - at max inventory (" << inventory << "/" << maxInventory << ")\n";
    }

    if (canSell()) {
        Order ask = createAsk(thisQuote.askPrice, size);
        currentAskId = ask.id; // set before process() so onTrade can attribute an immediate fill
        matchingEngine.process(ask);
        if (verbose) {
            printOrder(ask);
        }
    } else if (verbose) {
        std::cout << "  ask suppressed - at min inventory (" << inventory << "/-" << maxInventory << ")\n";
    }
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
    std::cout << "  Exposure: " << getExposure() << "\n";
    std::cout << "  Adverse Selection Ratio: " << getAdverseSelectionRatio()
               << " (" << adverseFillCount << "/" << totalFillCount << ")\n";
}