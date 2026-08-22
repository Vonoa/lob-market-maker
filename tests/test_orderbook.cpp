#include "doctest/doctest.h"
#include "OrderBook.h"
#include "Order.h"

// doctest basics: TEST_CASE("name") { ... } declares one test. CHECK(expr)
// records a failure and keeps running the rest of the test; REQUIRE(expr)
// stops the test immediately on failure (use it when a later line would
// crash/be meaningless if this check failed, e.g. dereferencing a pointer
// you just asserted is non-null).

TEST_CASE("OrderBook: best bid/ask reflect price priority") {
    OrderBook book;
    book.addOrder(createOrder(OrderSide::BUY, 99.0, 5));
    book.addOrder(createOrder(OrderSide::BUY, 101.0, 5)); // higher bid, should become best
    book.addOrder(createOrder(OrderSide::BUY, 100.0, 5));

    book.addOrder(createOrder(OrderSide::SELL, 105.0, 5));
    book.addOrder(createOrder(OrderSide::SELL, 103.0, 5)); // lower ask, should become best
    book.addOrder(createOrder(OrderSide::SELL, 104.0, 5));

    CHECK(book.bestBid() == 101.0); // highest bid wins
    CHECK(book.bestAsk() == 103.0); // lowest ask wins
}

TEST_CASE("OrderBook: same price ties break by arrival order (time priority)") {
    // Inserted in the same order they're created, matching every real caller
    // in this codebase (MarketMaker::quote(), RandomTrader, HistoricalDataReplay
    // all add an order to the book immediately after creating it - id order and
    // book-arrival order are the same thing everywhere this actually runs).
    OrderBook book;
    Order first = createOrder(OrderSide::BUY, 100.0, 5);
    Order second = createOrder(OrderSide::BUY, 100.0, 5); // same price, arrives second
    book.addOrder(first);
    book.addOrder(second);

    REQUIRE(book.getBestBid() != nullptr);
    CHECK(book.getBestBid()->id == first.id); // the earlier arrival wins the tie
}

TEST_CASE("OrderBook: mutating through getBestAsk() leaves the correct residual resting") {
    OrderBook book;
    book.addOrder(createOrder(OrderSide::SELL, 100.0, 10));

    Order* ask = book.getBestAsk();
    REQUIRE(ask != nullptr);
    reduceOrderQuantity(*ask, 4); // simulates a partial fill, same as MatchingEngine does

    CHECK(book.getBestAsk()->quantity == 6); // the book's own copy reflects the reduction
    CHECK(book.bestAsk() == 100.0); // still resting at the same price, just smaller
}

TEST_CASE("OrderBook: cancelOrder removes only the targeted order") {
    OrderBook book;
    Order low = createOrder(OrderSide::BUY, 99.0, 5);
    Order mid = createOrder(OrderSide::BUY, 100.0, 5);
    Order high = createOrder(OrderSide::BUY, 101.0, 5);
    book.addOrder(low);
    book.addOrder(mid);
    book.addOrder(high);

    CHECK(book.cancelOrder(mid.id) == true);
    CHECK(book.cancelOrder(mid.id) == false); // already gone - second cancel finds nothing

    // Walk the remaining book in priority order and confirm exactly {high, low} are left,
    // in that order - proving `mid` is gone and nothing else was disturbed.
    REQUIRE(book.getBestBid() != nullptr);
    CHECK(book.getBestBid()->id == high.id);
    book.removeBestBid();
    REQUIRE(book.getBestBid() != nullptr);
    CHECK(book.getBestBid()->id == low.id);
    book.removeBestBid();
    CHECK(book.getBestBid() == nullptr);
}

TEST_CASE("OrderBook: getBidLevels/getAskLevels aggregate per price, best-first, capped at depth") {
    OrderBook book;
    // Two orders resting at the SAME price (100.0) - should aggregate into
    // one level with combined quantity and orderCount == 2.
    book.addOrder(createOrder(OrderSide::BUY, 100.0, 5));
    book.addOrder(createOrder(OrderSide::BUY, 100.0, 3));
    book.addOrder(createOrder(OrderSide::BUY, 99.0, 10));
    book.addOrder(createOrder(OrderSide::BUY, 98.0, 1));

    std::vector<BookLevel> levels = book.getBidLevels(2); // fewer than the 3 price levels that exist

    REQUIRE(levels.size() == 2); // capped at the requested depth, not the full book
    CHECK(levels[0].price == 100.0); // best (highest) price first
    CHECK(levels[0].totalQuantity == 8); // 5 + 3 aggregated
    CHECK(levels[0].orderCount == 2);
    CHECK(levels[1].price == 99.0);
    CHECK(levels[1].totalQuantity == 10);
    CHECK(levels[1].orderCount == 1);
    // the 98.0 level is excluded - depth was capped at 2

    // Asks: same idea, ascending price order this time (lowest ask first).
    book.addOrder(createOrder(OrderSide::SELL, 102.0, 4));
    book.addOrder(createOrder(OrderSide::SELL, 101.0, 6));

    std::vector<BookLevel> askLevels = book.getAskLevels(10); // more depth than exists
    REQUIRE(askLevels.size() == 2); // returns fewer than requested, doesn't pad or error
    CHECK(askLevels[0].price == 101.0);
    CHECK(askLevels[1].price == 102.0);
}

TEST_CASE("OrderBook: empty book reports no liquidity on both sides") {
    OrderBook book;

    CHECK(book.bestBid() == 0.0);
    CHECK(book.bestAsk() == 0.0);
    CHECK(book.spread() == 0.0);
    CHECK(book.getBestBid() == nullptr);
    CHECK(book.getBestAsk() == nullptr);
    CHECK(book.cancelOrder(12345) == false);
}
