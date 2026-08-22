#include "OrderBook.h"
#include "Logging.h"
#include <algorithm>
#include <iostream>

namespace {
// bids and asks are different types (opposite comparators - see the note in
// cancelOrder()), so this is a template rather than a plain function taking
// one shared reference type. Both maps have the same shape (price -> deque
// of resting orders), which is all this needs.
template <typename LevelMap>
std::vector<BookLevel> topLevels(const LevelMap& levels, size_t depth) {
    std::vector<BookLevel> result;
    for (auto it = levels.begin(); it != levels.end() && result.size() < depth; ++it) {
        int64_t total = 0;
        for (const Order& order : it->second) {
            total += order.quantity;
        }
        result.push_back(BookLevel{it->first, total, static_cast<int>(it->second.size())});
    }
    return result;
}
} // namespace

// Function to add an Order to the OrderBook
void OrderBook::addOrder(const Order& order) {
    if (order.side == OrderSide::BUY) {
        bids[order.price].push_back(order);
    } else {
        asks[order.price].push_back(order);
    }
    index[order.id] = OrderLocation{order.side, order.price};
}

Order* OrderBook::getBestBid() {
    if (bids.empty()) {
        return nullptr;
    }
    return &bids.begin()->second.front();
}

Order* OrderBook::getBestAsk() {
    if (asks.empty()) {
        return nullptr;
    }
    return &asks.begin()->second.front();
}

// Function to get the best bid price
double OrderBook::bestBid() const {
    if (bids.empty()) {
        return 0.0; // No bids available
    }
    return bids.begin()->first; // the map key is the price itself
}

// Function to get the best ask price
double OrderBook::bestAsk() const {
    if (asks.empty()) {
        return 0.0; // No asks available
    }
    return asks.begin()->first;
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

std::vector<BookLevel> OrderBook::getBidLevels(size_t depth) const {
    return topLevels(bids, depth);
}

std::vector<BookLevel> OrderBook::getAskLevels(size_t depth) const {
    return topLevels(asks, depth);
}

void OrderBook::removeBestAsk() {
    if (asks.empty()) {
        return;
    }
    auto levelIt = asks.begin();
    std::deque<Order>& level = levelIt->second;
    index.erase(level.front().id);
    level.pop_front();
    if (level.empty()) {
        asks.erase(levelIt); // don't leave an empty price level behind - getBestAsk()
                              // must never see a level with nothing in it
    }
}

void OrderBook::removeBestBid() {
    if (bids.empty()) {
        return;
    }
    auto levelIt = bids.begin();
    std::deque<Order>& level = levelIt->second;
    index.erase(level.front().id);
    level.pop_front();
    if (level.empty()) {
        bids.erase(levelIt);
    }
}

// Function to print all Orders in the OrderBook
void OrderBook::printOrders() const {
    std::cout << "Bids:" << std::endl;
    for (const auto& [price, level] : bids) {
        for (const auto& order : level) {
            printOrder(order);
        }
    }
    std::cout << "Asks:" << std::endl;
    for (const auto& [price, level] : asks) {
        for (const auto& order : level) {
            printOrder(order);
        }
    }
}

bool OrderBook::cancelOrder(int orderId) {
    auto indexIt = index.find(orderId);
    if (indexIt == index.end()) {
        if (Logging::currentLevel >= LogLevel::Debug) {
            std::cout << "Order with ID " << orderId << " not found." << std::endl;
        }
        return false;
    }

    OrderLocation loc = indexIt->second;

    // bids and asks are different types (opposite comparators), so they can't be
    // unified behind one reference the way a ternary would - a generic lambda
    // (auto& sideMap) lets this same erase logic work for either map without
    // duplicating it per side.
    auto eraseFromLevel = [orderId, &loc](auto& sideMap) {
        auto levelIt = sideMap.find(loc.price); // must exist - index and the maps stay in sync everywhere
        std::deque<Order>& level = levelIt->second;
        auto orderIt = std::find_if(level.begin(), level.end(),
                                     [orderId](const Order& order) { return order.id == orderId; });
        level.erase(orderIt); // O(k) within this one price level, not the whole book
        if (level.empty()) {
            sideMap.erase(levelIt);
        }
    };

    if (loc.side == OrderSide::BUY) {
        eraseFromLevel(bids);
    } else {
        eraseFromLevel(asks);
    }

    index.erase(indexIt);

    if (Logging::currentLevel >= LogLevel::Debug) {
        std::cout << "Order with ID " << orderId << " canceled." << std::endl;
    }
    return true;
}
