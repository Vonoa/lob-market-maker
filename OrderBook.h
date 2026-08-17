#pragma once

#include <vector>
#include "Order.h"

// OrderBook class definition
class OrderBook {
private:
    std::vector<Order> bids;
    std::vector<Order> asks;

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

    void removeBestAsk();
    void removeBestBid();

    // Function to print all Orders in the OrderBook
    void printOrders() const;

    bool cancelOrder(int orderId);
};
