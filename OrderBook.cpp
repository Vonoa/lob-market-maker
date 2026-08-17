#include "OrderBook.h"
#include <algorithm>
#include <iostream>

// Function to add an Order to the OrderBook
void OrderBook::addOrder(const Order& order) {
    if (order.side == OrderSide::BUY) {
        bids.push_back(order);
    } else {
        asks.push_back(order);
    }
    std::sort(bids.begin(), bids.end(), [](const Order& a, const Order& b) {
        if (a.price != b.price) return a.price > b.price; // Highest price first
        return a.id < b.id; // Then earliest arrival first (FIFO) - id is assigned in creation order
    });
    std::sort(asks.begin(), asks.end(), [](const Order& a, const Order& b) {
        if (a.price != b.price) return a.price < b.price; // Lowest price first
        return a.id < b.id; // Then earliest arrival first (FIFO) - id is assigned in creation order
    });
}

Order* OrderBook::getBestBid() {
    if (!bids.empty()) {
        return &bids.front();
    }
    return nullptr;
}

Order* OrderBook::getBestAsk() {
    if (!asks.empty()) {
        return &asks.front();
    }
    return nullptr;
}

// Function to get the best bid price
double OrderBook::bestBid() const {
    if (!bids.empty()) {
        return bids.front().price;
    }
    return 0.0; // No bids available
}

// Function to get the best ask price
double OrderBook::bestAsk() const {
    if (!asks.empty()) {
        return asks.front().price;
    }
    return 0.0; // No asks available
}

// Function to calculate the spread between best ask and best bid
double OrderBook::spread() const {
    double bid = bestBid();
    double ask = bestAsk();
    if (bid > 0 && ask > 0) {
        return ask - bid;
    }
    return 0.0; // No valid spread if either bid or ask is missing
}

void OrderBook::removeBestAsk() {
    if (!asks.empty()) {
        asks.erase(asks.begin());
    }
}

void OrderBook::removeBestBid() {
    if (!bids.empty()) {
        bids.erase(bids.begin());
    }
}

// Function to print all Orders in the OrderBook
void OrderBook::printOrders() const {
    std::cout << "Bids:" << std::endl;
    for (const auto& order : bids) {
        printOrder(order);
    }
    std::cout << "Asks:" << std::endl;
    for (const auto& order : asks) {
        printOrder(order);
    }
}

bool OrderBook::cancelOrder(int orderId) {
    auto removeOrder = [orderId](std::vector<Order>& orders) {
        auto it = std::remove_if(orders.begin(), orders.end(),
                                 [orderId](const Order& order) { return order.id == orderId; });
        if (it == orders.end()) {
            return false; // Order not found
        }
        orders.erase(it, orders.end());
        return true; // Order found and removed
    };

    bool canceled = removeOrder(bids) || removeOrder(asks);

    if (canceled) {
        std::cout << "Order with ID " << orderId << " canceled." << std::endl;
    } else {
        std::cout << "Order with ID " << orderId << " not found." << std::endl;
    }

    return canceled;
}
