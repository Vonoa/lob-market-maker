#include <vector>

#include "doctest/doctest.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "Order.h"

TEST_CASE("MatchingEngine: a marketable buy walks multiple ask levels in price order") {
    OrderBook book;
    MatchingEngine engine(book);
    std::vector<Trade> trades;
    engine.setOnTrade([&trades](const Trade& trade) { trades.push_back(trade); });

    book.addOrder(createOrder(OrderSide::SELL, 100.0, 5));
    book.addOrder(createOrder(OrderSide::SELL, 101.0, 5));

    Order buy = createOrder(OrderSide::BUY, 102.0, 8); // marketable against both levels
    engine.process(buy);

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].price == 100.0); // cheaper level first
    CHECK(trades[0].quantity == 5);
    CHECK(trades[1].price == 101.0);
    CHECK(trades[1].quantity == 3); // only 3 of the 8 remained after the first level
}

TEST_CASE("MatchingEngine: a non-marketable order rests instead of trading") {
    OrderBook book;
    MatchingEngine engine(book);
    std::vector<Trade> trades;
    engine.setOnTrade([&trades](const Trade& trade) { trades.push_back(trade); });

    book.addOrder(createOrder(OrderSide::SELL, 100.0, 5));

    Order buy = createOrder(OrderSide::BUY, 99.0, 5); // doesn't cross the ask
    engine.process(buy);

    CHECK(trades.empty());
    CHECK(book.bestBid() == 99.0); // it rested as a new bid instead
}

TEST_CASE("MatchingEngine: the aggressor trades at the resting order's price, not its own") {
    OrderBook book;
    MatchingEngine engine(book);
    std::vector<Trade> trades;
    engine.setOnTrade([&trades](const Trade& trade) { trades.push_back(trade); });

    book.addOrder(createOrder(OrderSide::SELL, 100.0, 5));

    Order buy = createOrder(OrderSide::BUY, 105.0, 5); // willing to pay more than needed
    engine.process(buy);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].price == 100.0); // fills at the resting ask's price, not the bid's 105
}

TEST_CASE("MatchingEngine: total traded quantity is min(aggressor qty, available liquidity)") {
    OrderBook book;
    MatchingEngine engine(book);
    std::vector<Trade> trades;
    engine.setOnTrade([&trades](const Trade& trade) { trades.push_back(trade); });

    book.addOrder(createOrder(OrderSide::SELL, 100.0, 5));
    book.addOrder(createOrder(OrderSide::SELL, 101.0, 3)); // total liquidity = 8

    Order buy = createOrder(OrderSide::BUY, 200.0, 20); // wants far more than is available
    engine.process(buy);

    int64_t totalTraded = 0;
    for (const auto& trade : trades) {
        totalTraded += trade.quantity;
    }
    CHECK(totalTraded == 8); // capped by available liquidity, not the aggressor's requested size

    // The unmatched remainder (20 - 8 = 12) should rest as a new bid.
    CHECK(book.bestBid() == 200.0);
}
