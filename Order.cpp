#include "Order.h"
#include <iostream>

// Creating a Order
Order createOrder(OrderSide side, double price, int quantity) {
    static int nextId = 1;

    Order order;
    order.id = nextId++;
    order.side = side;
    order.price = price;
    order.quantity = quantity;

    return order;
}

// Function to reduce the quantity of an Order
void reduceOrderQuantity(Order& order, int amount) {
    if (amount < 0) {
        std::cerr << "Error: Cannot reduce by a negative quantity." << std::endl;
        return;
    }
    if (amount > order.quantity) {
        std::cerr << "Error: Cannot reduce by more than the current quantity." << std::endl;
        return;
    }
    order.quantity -= amount;
}

// Function to print Order details
void printOrder(const Order& order) {
    std::string sideStr = (order.side == OrderSide::BUY) ? "BUY" : "SELL";
    std::cout << "Order ID: " << order.id << ", Side: " << sideStr
              << ", Price: " << order.price << ", Quantity: " << order.quantity << std::endl;
}
