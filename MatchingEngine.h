#pragma once

#include <functional>
#include <vector>
#include "Order.h"
#include "OrderBook.h"

class MatchingEngine {
private:
    OrderBook& orderBook;
    std::vector<Trade> trades;
    int nextTradeId = 1;
    std::function<void(const Trade&)> onTrade;

public:
    explicit MatchingEngine(OrderBook& orderBook);

    void process(Order order);
    void printTrades() const;
    void setOnTrade(std::function<void(const Trade&)> callback);
};
