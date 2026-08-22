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
};

// Trade structure definition
struct Trade {
    int id;
    int buyOrderId;
    int sellOrderId;
    double price;
    int64_t quantity;
};

// Creating a Order. The order's id is assigned internally and is guaranteed unique.
Order createOrder(OrderSide side, double price, int64_t quantity);

// Function to reduce the quantity of an Order
void reduceOrderQuantity(Order& order, int64_t amount);

// Function to print Order details
void printOrder(const Order& order);
