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
    int totalFillCount = 0;

public:
    MarketMaker(OrderBook& orderBook, MatchingEngine& matchingEngine, Strategy& strategy,
                int64_t maxInventory = 100, int64_t maxOrderSize = 50,
                double maxloss = 1000.0, double maxExposure = 5000.0);

    Order createBid(double price, int64_t quantity) const;
    Order createAsk(double price, int64_t quantity) const;

    double calcMidPrice() const;

    void quote(int64_t quantity);
    void cancelQuotes();
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
};
