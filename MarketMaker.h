#pragma once

#include "Order.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "FairValueModel.h"

class MarketMaker {
private:
    OrderBook& orderBook;
    MatchingEngine& matchingEngine;
    FairValueModel fairValueModel;

    static constexpr int NO_QUOTE_ID = -1;
    int currentBidId;
    int currentAskId;
    int inventory = 0;
    int maxInventory = 100;
    int maxOrderSize = 50;
    double skewCoefficient = 0.004;
    double startingCash = 100000.0; // Starting cash for the market maker
    double cash = startingCash; // Current cash balance for the market maker
    double avgCostBasis = 0.0; // Weighted-average price of the current open position
    double realisedPnL = 0.0; // Realized profit and loss from closed positions
    double unrealisedPnL = 0.0; // Unrealized profit and loss from open positions
    bool killswitch = false; // If true, the market maker will not quote new orders
    double maxloss = 1000.0; // Maximum loss threshold for the market maker
    double maxExposure = 1000.0;
    double volatilityMultiplier = 1.0;

public:
    MarketMaker(OrderBook& orderBook, MatchingEngine& matchingEngine);

    Order createBid(int quantity) const;
    Order createAsk(int quantity) const;

    double calcMidPrice() const;

    void quote(int quantity);
    void cancelQuotes();
    int getInventory() const;
    bool canBuy() const;
    bool canSell() const;
    void updateInventory(int quantityChange, OrderSide side);
    void updateCostBasis(int quantity, double price, OrderSide side);
    double getAvgCostBasis() const;
    double getRealisedPnL() const;
    void onTrade(const Trade& trade);
    double reservationPrice() const;
    double getUnrealisedPnL() const;
    double getTotalPnL() const;
    void printStatus() const;
    void resetKillSwitch();
    double getExposure() const;
    double effectiveSpread() const;
};
