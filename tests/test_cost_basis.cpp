#include "doctest/doctest.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "MarketMaker.h"
#include "DefaultStrategy.h"
#include "Order.h"

// updateCostBasis()/updateInventory() are public specifically so they can be
// tested directly, without needing a full quote()/fill cycle - but the header
// comment on updateCostBasis() is explicit that call order matters: it must
// run BEFORE updateInventory() for the same fill, since it needs to see the
// pre-fill position to decide whether this is opening, adding to, reducing,
// or flipping a position. Every test below follows that same two-call order,
// matching exactly what MarketMaker::onTrade() itself does.

TEST_CASE("Cost basis: flat -> long -> flat round trip books the right realised PnL") {
    OrderBook book;
    MatchingEngine engine(book);
    DefaultStrategy strategy;
    MarketMaker mm(book, engine, strategy);

    mm.updateCostBasis(10, 100.0, OrderSide::BUY); // open long @ 100
    mm.updateInventory(10, OrderSide::BUY);
    CHECK(mm.getAvgCostBasis() == 100.0);
    CHECK(mm.getInventory() == 10);

    mm.updateCostBasis(10, 105.0, OrderSide::SELL); // close it @ 105
    mm.updateInventory(10, OrderSide::SELL);

    CHECK(mm.getInventory() == 0);
    CHECK(mm.getRealisedPnL() == doctest::Approx(50.0)); // (105 - 100) * 10
}

TEST_CASE("Cost basis: adding to a position produces a size-weighted average cost") {
    OrderBook book;
    MatchingEngine engine(book);
    DefaultStrategy strategy;
    MarketMaker mm(book, engine, strategy);

    mm.updateCostBasis(10, 100.0, OrderSide::BUY);
    mm.updateInventory(10, OrderSide::BUY);

    mm.updateCostBasis(10, 110.0, OrderSide::BUY); // add 10 more @ 110
    mm.updateInventory(10, OrderSide::BUY);

    CHECK(mm.getInventory() == 20);
    CHECK(mm.getAvgCostBasis() == doctest::Approx(105.0)); // (100*10 + 110*10) / 20
}

TEST_CASE("Cost basis: reducing a position leaves avgCostBasis unchanged") {
    OrderBook book;
    MatchingEngine engine(book);
    DefaultStrategy strategy;
    MarketMaker mm(book, engine, strategy);

    mm.updateCostBasis(10, 100.0, OrderSide::BUY);
    mm.updateInventory(10, OrderSide::BUY);

    mm.updateCostBasis(4, 120.0, OrderSide::SELL); // partial close, doesn't flip
    mm.updateInventory(4, OrderSide::SELL);

    CHECK(mm.getInventory() == 6);
    CHECK(mm.getAvgCostBasis() == doctest::Approx(100.0)); // unchanged by the partial close
    CHECK(mm.getRealisedPnL() == doctest::Approx(80.0)); // (120 - 100) * 4
}

TEST_CASE("Cost basis: flipping long to short in one fill splits realised PnL from the new basis") {
    OrderBook book;
    MatchingEngine engine(book);
    DefaultStrategy strategy;
    MarketMaker mm(book, engine, strategy);

    mm.updateCostBasis(10, 100.0, OrderSide::BUY); // open long 10 @ 100
    mm.updateInventory(10, OrderSide::BUY);

    mm.updateCostBasis(15, 110.0, OrderSide::SELL); // sell 15: closes the 10, opens -5 short
    mm.updateInventory(15, OrderSide::SELL);

    CHECK(mm.getInventory() == -5);
    CHECK(mm.getRealisedPnL() == doctest::Approx(100.0)); // (110 - 100) * 10, on the closed portion only
    CHECK(mm.getAvgCostBasis() == doctest::Approx(110.0)); // remaining 5 opened fresh at the fill price
}

TEST_CASE("PnL invariant: realised + unrealised equals cash + inventory*mid - startingCash") {
    OrderBook book;
    MatchingEngine engine(book);
    DefaultStrategy strategy;
    // Generous limits so the kill switch can't interfere with this test.
    MarketMaker mm(book, engine, strategy, 1000, 1000, 1e9, 1e9);

    engine.setOnTrade([&mm](const Trade& trade) { mm.onTrade(trade); });

    // Seed a two-sided market so quote() has something to center on.
    book.addOrder(createOrder(OrderSide::BUY, 99.0, 10));
    book.addOrder(createOrder(OrderSide::SELL, 101.0, 10));

    mm.quote(10); // rests a bid and ask around the ~100 mid

    // An aggressive incoming buy, priced well above anything resting, is
    // guaranteed to cross and fill the market maker's resting ask.
    Order aggressiveBuy = createOrder(OrderSide::BUY, 1000.0, 5);
    engine.process(aggressiveBuy);

    double totalPnl = mm.getRealisedPnL() + mm.getUnrealisedPnL();
    double checkTotalPnl = mm.getCash() + static_cast<double>(mm.getInventory()) * mm.calcMidPrice() - 100000.0;

    CHECK(totalPnl == doctest::Approx(checkTotalPnl).epsilon(0.01));
}
