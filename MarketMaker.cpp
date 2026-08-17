#include "MarketMaker.h"
#include "FairValueModel.h"
#include <algorithm>
#include <iostream>

MarketMaker::MarketMaker(OrderBook& orderBook, MatchingEngine& matchingEngine)
    : orderBook(orderBook), matchingEngine(matchingEngine),
      currentBidId(NO_QUOTE_ID), currentAskId(NO_QUOTE_ID) {}

double MarketMaker::calcMidPrice() const {
    double bestBid = orderBook.bestBid();
    double bestAsk = orderBook.bestAsk();

    if (bestBid == 0.0 || bestAsk == 0.0) {
        std::cerr << "Error: Cannot calculate mid-price with no bids or asks." << std::endl;
        return 0.0;
    }

    return (bestBid + bestAsk) / 2.0;
}

double MarketMaker::reservationPrice() const {
    return calcMidPrice() - skewCoefficient * inventory / 2.0;
}

double MarketMaker::effectiveSpread() const {
    return orderBook.spread() + volatilityMultiplier * fairValueModel.getVolatility();
}

Order MarketMaker::createBid(int quantity) const {
    double mid = reservationPrice();
    double bidPrice = mid - effectiveSpread() / 2.0; // Place bid below mid-price

    return createOrder(OrderSide::BUY, bidPrice, quantity);
}

Order MarketMaker::createAsk(int quantity) const {
    double mid = reservationPrice();
    double askPrice = mid + effectiveSpread() / 2.0; // Place ask above mid-price

    return createOrder(OrderSide::SELL, askPrice, quantity);
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

int MarketMaker::getInventory() const {
    return inventory;
}

bool MarketMaker::canBuy() const {
    return inventory < maxInventory;
}

bool MarketMaker::canSell() const {
    return inventory > -maxInventory;
}

void MarketMaker::updateInventory(int quantityChange, OrderSide side) {
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
void MarketMaker::updateCostBasis(int quantity, double price, OrderSide side) {
    int dir = (side == OrderSide::BUY) ? 1 : -1;

    if (inventory == 0) {
        // Flat -> opening a fresh position: the fill price is the new cost basis.
        avgCostBasis = price;
        return;
    }

    int positionSign = (inventory > 0) ? 1 : -1;

    if (dir == positionSign) {
        // Adding to the existing position: roll the fill into a size-weighted average.
        int absInventory = std::abs(inventory);
        avgCostBasis = (avgCostBasis * absInventory + price * quantity) / (absInventory + quantity);
    } else {
        // Reducing (or flipping) the position.
        int closingQty = std::min(quantity, std::abs(inventory));

        // Booked here, not in onTrade(): this is the only place that knows this fill is
        // actually closing exposure (vs. opening/adding), and has the pre-fill avgCostBasis.
        realisedPnL += positionSign * (price - avgCostBasis) * closingQty;

        int remainingQty = quantity - closingQty;
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
    if (std::abs(totalPnl - checkTotalPnl) > 1e-6) {
        std::cerr << "Warning: Total PnL mismatch! TotalPnL: " << totalPnl << ", CheckTotalPnl: " << checkTotalPnl << std::endl;
    }
    return totalPnl;
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

    if (filledBid && filledAsk) {
        std::cerr << "Warning: self-trade between MarketMaker's own bid (" << currentBidId
                   << ") and ask (" << currentAskId << "); inventory left unchanged." << std::endl;
        return;
    }

    if (filledBid) {
        updateCostBasis(trade.quantity, trade.price, OrderSide::BUY);
        updateInventory(trade.quantity, OrderSide::BUY);
        cash -= trade.quantity * trade.price;
    }
    if (filledAsk) {
        updateCostBasis(trade.quantity, trade.price, OrderSide::SELL);
        updateInventory(trade.quantity, OrderSide::SELL);
        cash += trade.quantity * trade.price;
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

void MarketMaker::quote(int quantity) {
    if (killswitch) {
        std::cout << "Market Maker halted - kill switch active. Call resetKillSwitch() to resume.\n";
        return;
    }

    if (orderBook.bestBid() == 0.0 || orderBook.bestAsk() == 0.0) {
        std::cerr << "Error: Cannot quote without an existing two-sided market." << std::endl;
        return;
    }
    int size = std::min(quantity, maxOrderSize); // clamp to the max single-order size rather than refusing to quote at all

    cancelQuotes();

    std::cout << "Market Maker Quotes:\n";

    // Route through the matching engine (not orderBook.addOrder directly) so a quote
    // that crosses the touch - e.g. once skew shifts it off center - actually executes
    // instead of resting uncrossed in the book.
    if (canBuy()) {
        Order bid = createBid(size);
        currentBidId = bid.id; // set before process() so onTrade can attribute an immediate fill
        matchingEngine.process(bid);
        printOrder(bid);
    } else {
        std::cout << "  bid suppressed - at max inventory (" << inventory << "/" << maxInventory << ")\n";
    }

    if (canSell()) {
        Order ask = createAsk(size);
        currentAskId = ask.id; // set before process() so onTrade can attribute an immediate fill
        matchingEngine.process(ask);
        printOrder(ask);
    } else {
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
}