#include <iostream>

#include "Order.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "MarketMaker.h"

int main() {

    OrderBook orderBook;
    MatchingEngine engine(orderBook);

    // Existing market
    orderBook.addOrder(
        createOrder(OrderSide::BUY, 99.90, 10)
    );

    orderBook.addOrder(
        createOrder(OrderSide::SELL, 100.10, 10)
    );

    MarketMaker marketMaker(orderBook, engine);

    // Feed every trade back to the market maker so it can track fills against its own quote
    engine.setOnTrade([&marketMaker](const Trade& trade) {
        marketMaker.onTrade(trade);
    });

    double mid = marketMaker.calcMidPrice();
    std::cout << "Mid Price: " << mid << std::endl;

    marketMaker.quote(10);

    std::cout << "Inventory before: " << marketMaker.getInventory() << std::endl;

    // Incoming taker order sweeps the existing ask, then fills into the MM's own ask
    engine.process(createOrder(OrderSide::BUY, 100.10, 15));

    std::cout << "Inventory after: " << marketMaker.getInventory() << std::endl;

    engine.printTrades();

    marketMaker.printStatus();

    return 0;
}
