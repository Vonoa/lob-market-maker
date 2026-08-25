#pragma once

#include <cstdint>
#include <string>

// Enum class for Order Side
enum class OrderSide {
    BUY,
    SELL
};

// Order structure definition
struct Order {
    int id; // Globally unique, assigned by createOrder(); also doubles as arrival order for FIFO tie-breaking
    OrderSide side;
    double price;
    int64_t quantity;
    int64_t timestampNs = 0; // 0 = unset/unknown; real value set by the caller (see createOrder())
};

// Trade structure definition
struct Trade {
    int id;
    int buyOrderId;
    int sellOrderId;
    double price;
    int64_t quantity;
    int64_t timestampNs = 0;                  // stamped from the aggressor order at match time
    OrderSide aggressorSide = OrderSide::BUY; // side of the order that crossed the book, not the resting side
};

// Creating a Order. The order's id is assigned internally and is guaranteed unique.
// timestampNs defaults to 0 (unknown) so existing callers without a real clock yet
// don't need to change.
Order createOrder(OrderSide side, double price, int64_t quantity, int64_t timestampNs = 0);

// Function to reduce the quantity of an Order
void reduceOrderQuantity(Order& order, int64_t amount);

// Function to print Order details
void printOrder(const Order& order);
