#pragma once

#include <deque>
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

#include "Order.h"

// One row of a depth ladder - a price level with everything resting there
// aggregated together. Doesn't distinguish whose orders they are (that's
// still just id/quantity inside the book) - a GUI wanting to highlight its
// own resting orders separately does that by comparing ids itself.
struct BookLevel {
    double price;
    int64_t totalQuantity;
    int orderCount;
};

// OrderBook class definition
class OrderBook {
private:
    // Bids ordered highest-price-first (best bid = begin()); asks ordered
    // lowest-price-first (best ask = begin()). Each price level is a FIFO
    // deque - push_back on arrival, pop_front removes the oldest (earliest
    // id, i.e. highest time-priority) order at that price. This replaces a
    // full std::sort of the whole side on every insert with an O(log levels)
    // map insertion plus an O(1) amortized push_back.
    std::map<double, std::deque<Order>, std::greater<double>> bids;
    std::map<double, std::deque<Order>> asks;

    // order id -> which side/price it's resting at, so cancelOrder() doesn't
    // need to search every price level. O(1) to find the right level, O(k)
    // to erase within it (k = orders resting at that exact price - usually
    // small, and always far less than a full-book scan).
    struct OrderLocation {
        OrderSide side;
        double price;
    };
    std::unordered_map<int, OrderLocation> index;

public:
    // Function to add an Order to the OrderBook
    void addOrder(const Order& order);

    Order* getBestBid();
    Order* getBestAsk();

    // Function to get the best bid price
    double bestBid() const;

    // Function to get the best ask price
    double bestAsk() const;

    // Function to calculate the spread between best ask and best bid
    double spread() const;

    // Top `depth` price levels, best-first (highest bid / lowest ask, same
    // ordering the book itself already keeps). Returns fewer than `depth`
    // if the book doesn't have that many levels resting.
    std::vector<BookLevel> getBidLevels(size_t depth) const;
    std::vector<BookLevel> getAskLevels(size_t depth) const;

    void removeBestAsk();
    void removeBestBid();

    // Function to print all Orders in the OrderBook
    void printOrders() const;

    bool cancelOrder(int orderId);
};
