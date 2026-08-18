#include <iostream>
#include <vector>
#include <cmath>

#include "Order.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "MarketMaker.h"
#include "RandomTrader.h"
#include "DefaultStrategy.h"
#include "FixedSpreadStrategy.h"
#include "HistoricalDataReplay.h"

// Replays real trade data instead of RandomTrader's synthetic flow. Note the risk
// parameters passed to MarketMaker here are BTC-scale, not the ~$100 synthetic-sim
// defaults - see the scale-mismatch discussion (quantity scaling inflates exposure/PnL
// by 1,000,000x, and BTC's real price/size scale is totally different from ~$100 stocks).
double runHistoricalSimulation(const std::string& filepath, int maxRows, Strategy& strategy) {
    OrderBook orderBook;
    MatchingEngine engine(orderBook);

    // Seed near the real starting price for this file, not the ~100 synthetic placeholder
    orderBook.addOrder(createOrder(OrderSide::BUY, 62899.50, 2000));
    orderBook.addOrder(createOrder(OrderSide::SELL, 62900.50, 2000));

    MarketMaker marketMaker(orderBook, engine, strategy,
                             50000, 5000, 1000000000.0, 5000000000.0);
    HistoricalDataReplay replay(filepath);

    engine.setOnTrade([&marketMaker](const Trade& trade) {
        marketMaker.onTrade(trade);
    });

    int rowsProcessed = 0;
    Order historicalOrder;
    while (rowsProcessed < maxRows && replay.nextOrder(historicalOrder)) {
        marketMaker.quote(2000);
        engine.process(historicalOrder);
        rowsProcessed++;
    }

    return marketMaker.getTotalPnL();
}

double runSimulation(unsigned int seed, Strategy& strategy){

    OrderBook orderBook;
    MatchingEngine engine(orderBook);

    // Seed an initial two-sided market so quote() has something to center on
    orderBook.addOrder(createOrder(OrderSide::BUY, 99.90, 10));
    orderBook.addOrder(createOrder(OrderSide::SELL, 100.10, 10));

    MarketMaker marketMaker(orderBook, engine, strategy);
    RandomTrader randomTrader(orderBook, seed);

    // Feed every trade back to the market maker so it can track fills against its own quotes
    engine.setOnTrade([&marketMaker](const Trade& trade) {
        marketMaker.onTrade(trade);
    });

    const int numRounds = 100;
    for (int round = 0; round < numRounds; ++round) {
        marketMaker.quote(10);

        if (randomTrader.shouldTrade()) {
            Order randomOrder = randomTrader.generateOrder();
            engine.process(randomOrder);
        }
    }

    return marketMaker.getTotalPnL();
}

std::vector<double> runMonteCarlo(Strategy& strategy, int numSims) {
    std::vector<double> results;
    for (unsigned int seed = 0; seed < static_cast<unsigned int>(numSims); ++seed) {
        results.push_back(runSimulation(seed, strategy));
    }
    return results;
}

void printStats(const std::string& label, const std::vector<double>& results) {
    // Mean - same two-pass shape as FairValueModel::getVolatility(), just over
    // final PnL outcomes instead of price differences.
    double sum = 0.0;
    for (double pnl : results) {
        sum += pnl;
    }
    double mean = sum / results.size();

    // Standard deviation - second pass, using the mean from the first.
    double sumSquaredDeviations = 0.0;
    for (double pnl : results) {
        double deviation = pnl - mean;
        sumSquaredDeviations += deviation * deviation;
    }
    double variance = sumSquaredDeviations / results.size();
    double stdDev = std::sqrt(variance);

    // Win rate - fraction of runs that ended up profitable at all.
    int wins = 0;
    for (double pnl : results) {
        if (pnl > 0.0) {
            wins++;
        }
    }
    double winRate = static_cast<double>(wins) / results.size();

    std::cout << "--- " << label << " ---\n";
    std::cout << "Mean Total PnL:      " << mean << "\n";
    std::cout << "Std Dev of PnL:      " << stdDev << "\n";
    std::cout << "Win Rate:            " << (winRate * 100.0) << "%\n\n";
}

int main() {
    const int numSims = 100;

    DefaultStrategy defaultStrategy;
    FixedSpreadStrategy fixedSpreadStrategy;

    std::cout << "=== Synthetic Monte Carlo (" << numSims << " runs each) ===\n";
    printStats("DefaultStrategy", runMonteCarlo(defaultStrategy, numSims));
    printStats("FixedSpreadStrategy", runMonteCarlo(fixedSpreadStrategy, numSims));

    std::cout << "=== Historical replay (BTCUSDT, first 1000 rows) ===\n";
    double defaultHistPnL = runHistoricalSimulation("BTCUSDT-trades-2026-08-17.csv", 1000, defaultStrategy);
    std::cout << "DefaultStrategy     Total PnL (real $): " << defaultHistPnL / 1000000.0 << "\n";
    double fixedHistPnL = runHistoricalSimulation("BTCUSDT-trades-2026-08-17.csv", 1000, fixedSpreadStrategy);
    std::cout << "FixedSpreadStrategy Total PnL (real $): " << fixedHistPnL / 1000000.0 << "\n";

    return 0;
}
