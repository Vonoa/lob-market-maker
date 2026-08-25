#include "MatchingEngine.h"
#include "Logging.h"
#include <algorithm>
#include <iostream>

MatchingEngine::MatchingEngine(OrderBook& orderBook) : orderBook(orderBook) {}

void MatchingEngine::setOnTrade(std::function<void(const Trade&)> callback) {
    onTrade = std::move(callback);
}

void MatchingEngine::process(Order order) {
    if (order.side == OrderSide::BUY) {
        while (order.quantity > 0) {
            Order* ask = orderBook.getBestAsk();

            if (ask == nullptr) {
                break;
            }

            if (order.price < ask->price) {
                break;
            }

            int64_t tradeQuantity = std::min(order.quantity, ask->quantity);

            Trade trade{
                nextTradeId,
                order.id,
                ask->id,
                ask->price,
                tradeQuantity,
                order.timestampNs,
                order.side
            };

            nextTradeId++;

            trades.push_back(trade);

            if (Logging::currentLevel >= LogLevel::Trades) {
                std::cout << "Trade executed:" << tradeQuantity << " @ " << ask->price << std::endl;
            }

            // Finish all bookkeeping on `ask` before notifying listeners - onTrade() may run
            // logic (e.g. a kill switch) that cancels orders, which can erase this very order
            // from the book and invalidate this pointer.
            reduceOrderQuantity(order, tradeQuantity);
            reduceOrderQuantity(*ask, tradeQuantity);

            if (ask->quantity == 0) {
                orderBook.removeBestAsk();
            }

            if (onTrade) {
                onTrade(trade);
            }
        }
    } else {
        while (order.quantity > 0) {
            Order* bid = orderBook.getBestBid();

            if (bid == nullptr) {
                break;
            }

            if (order.price > bid->price) {
                break;
            }

            int64_t tradeQuantity = std::min(order.quantity, bid->quantity);

            Trade trade{
                nextTradeId,
                bid->id,
                order.id,
                bid->price,
                tradeQuantity,
                order.timestampNs,
                order.side
            };

            nextTradeId++;

            trades.push_back(trade);

            if (Logging::currentLevel >= LogLevel::Trades) {
                std::cout << "Trade executed:" << tradeQuantity << " @ " << bid->price << std::endl;
            }

            // Finish all bookkeeping on `bid` before notifying listeners - onTrade() may run
            // logic (e.g. a kill switch) that cancels orders, which can erase this very order
            // from the book and invalidate this pointer.
            reduceOrderQuantity(order, tradeQuantity);
            reduceOrderQuantity(*bid, tradeQuantity);

            if (bid->quantity == 0) {
                orderBook.removeBestBid();
            }

            if (onTrade) {
                onTrade(trade);
            }
        }
    }

    if (order.quantity > 0) {
        orderBook.addOrder(order);
    }
}

void MatchingEngine::printTrades() const {
    std::cout << "\n Trades \n";
    for (const auto& trade : trades) {
        std::cout << "Trade " << trade.id
                    << " | Buy Order ID " << trade.buyOrderId
                    << " | Sell Order ID " << trade.sellOrderId
                    << " | Price: " << trade.price
                    << " | Quantity: " << trade.quantity << std::endl;
    }
}
