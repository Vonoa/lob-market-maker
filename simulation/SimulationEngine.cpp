#include "SimulationEngine.h"

// Member initializer list order follows DECLARATION order in the header
// (config, strategy, orderBook, matchingEngine, marketMaker, ...), not the
// order written here - orderBook and matchingEngine must already exist
// before marketMaker's constructor runs, since it takes references to both.
SimulationEngine::SimulationEngine(SimConfig config, Strategy& strategy)
    : config(config), strategy(strategy),
      orderBook(), matchingEngine(orderBook),
      marketMaker(orderBook, matchingEngine, strategy,
                   config.maxInventory, config.maxOrderSize,
                   config.maxLoss, config.maxExposure) {

    // Trade-based reference pricing (see MarketMaker::setUseTradeBasedReference)
    // is scoped to Historical specifically - it fixes a feedback loop that's
    // only real at BTC scale, where quoteSize dwarfs typical trade sizes and
    // the MM's own resting orders can dominate the book. At Synthetic's ~$100
    // scale quoteSize is comparable to real order sizes, so the live book is
    // already a good reference and switching it just discards well-tuned
    // synthetic-scale behaviour (confirmed empirically - applying it there
    // dropped DefaultStrategy's Monte Carlo win rate from 81% to 35%).
    marketMaker.setUseTradeBasedReference(config.source == SimConfig::Source::Historical);

    if (config.source == SimConfig::Source::Synthetic) {
        // Matches runSimulation()'s synthetic ~$100 scale in main.cpp.
        orderBook.addOrder(createOrder(OrderSide::BUY, 99.90, 10));
        orderBook.addOrder(createOrder(OrderSide::SELL, 100.10, 10));
    } else {
        // Matches runHistoricalSimulation()'s BTC scale in main.cpp.
        orderBook.addOrder(createOrder(OrderSide::BUY, 62899.50, 2000));
        orderBook.addOrder(createOrder(OrderSide::SELL, 62900.50, 2000));
    }

    if (config.source == SimConfig::Source::Synthetic) {
        randomTrader = std::make_unique<RandomTrader>(orderBook, config.seed);
    } else {
        replay = std::make_unique<HistoricalDataReplay>(config.dataPath);
    }

    // Every fill needs to reach marketMaker.onTrade(), or inventory/PnL/kill-switch
    // tracking never updates. [this] gives the lambda access to all of *this's
    // members (including marketMaker), which matchingEngine.process() will invoke
    // internally the moment a trade actually executes.
    matchingEngine.setOnTrade([this](const Trade& trade) {
        marketMaker.onTrade(trade);
        tickHadFill = true;
        tickLastFillPrice = trade.price;
        tickLastFillQty = trade.quantity;
    });
}

bool SimulationEngine::step() {
    // Run length is bounded by config.maxTicks, matching the CLI loops this
    // replaced. Returning false is how the caller (CLI loop or GUI frame) is
    // told the run is over.
    if (currentTick >= config.maxTicks) {
        return false;
    }

    // Once the kill switch trips, MarketMaker stops quoting for good (no
    // auto-reset), but the order source (RandomTrader/HistoricalDataReplay)
    // has no idea that happened and would keep feeding orders into a book
    // with no market maker to trade against. Those orders end up pricing
    // themselves off bestBid()/bestAsk()'s 0.0 "empty book" sentinel as if
    // it were a real price, producing garbage near-zero trades that corrupt
    // the fair-value estimate for the rest of the run. Ending the run here
    // instead - once halted, stay halted - is what a kill switch is supposed
    // to mean.
    if (marketMaker.isKillSwitchActive()) {
        return false;
    }

    // Reset BEFORE this tick's quote()/process() run, not after - the onTrade
    // callback (set up in the constructor) sets these DURING process(), so
    // resetting afterward would immediately wipe out the very fill it just
    // recorded. This tick's flag needs to start clean, then get set (or not)
    // by whatever happens below.
    tickHadFill = false;
    tickLastFillPrice = 0.0;
    tickLastFillQty = 0;

    // The two order sources have deliberately different shapes, and the
    // difference matters: RandomTrader splits "does a trade happen this tick"
    // from "build it", so a quiet tick is a normal tick and the run continues.
    // HistoricalDataReplay answers both at once, and a false return means the
    // file is exhausted - i.e. the run is genuinely over. Collapsing the two
    // into one boolean would end every synthetic run at its first quiet tick.
    Order incomingOrder;
    bool haveOrder = false;

    if (config.source == SimConfig::Source::Synthetic) {
        // Quote FIRST here: RandomTrader::generateOrder() prices itself off the
        // live book (bestBid/bestAsk), so it must see THIS tick's fresh quotes,
        // not last tick's stale ones - matches the old runSimulation() loop's
        // actual order (quote(), then shouldTrade()/generateOrder()).
        marketMaker.quote(config.quoteSize);

        haveOrder = randomTrader->shouldTrade(); // decides IF a trade happens this tick
        if (haveOrder) {
            incomingOrder = randomTrader->generateOrder(); // only build one if shouldTrade() said yes
        }
    } else {
        // Get the order FIRST here: matches the old runHistoricalSimulation()'s
        // `while (... && replay.nextOrder(...))` short-circuit, which never calls
        // quote() on the tick the feed actually runs out.
        haveOrder = replay->nextOrder(incomingOrder); // out-parameter: fills incomingOrder, returns success
        if (!haveOrder) {
            return false; // feed exhausted, the run really is over
        }
        // incomingOrder.price is the raw historical exchange price, parsed
        // straight from the CSV before anything in this sim touches it - the
        // one genuinely external, non-self-referential price observation
        // available each tick. Recorded before quote() so this tick's
        // reservation price reflects it. See onTrade()'s comment for why this
        // has to replace (not just supplement) feeding fairValueModel off
        // trade.price in Historical mode.
        marketMaker.recordExternalPrice(incomingOrder.price);
        marketMaker.quote(config.quoteSize);
    }

    if (haveOrder) {
        matchingEngine.process(incomingOrder);
    }

    currentTick++;
    refreshSnapshot();
    return true;
}

void SimulationEngine::refreshSnapshot() {
    // One flat copy of engine state into the snapshot the GUI and recorder both
    // read. Deliberately a copy rather than handing out references into live
    // engine internals - consumers can hold a snapshot across frames without
    // it mutating underneath them mid-render.

    latestSnapshot.tick = currentTick;

    latestSnapshot.bestBid = orderBook.bestBid();
    latestSnapshot.bestAsk = orderBook.bestAsk();
    latestSnapshot.spread = orderBook.spread();

    latestSnapshot.bidLevels = orderBook.getBidLevels(10);
    latestSnapshot.askLevels = orderBook.getAskLevels(10);

    // Uses calcMidPrice(), not (bestBid+bestAsk)/2 - calcMidPrice() already has
    // the fair-value fallback for a one-sided book, so recomputing it here would
    // reintroduce the exact bug that fallback was built to fix.
    latestSnapshot.mid = marketMaker.calcMidPrice();

    latestSnapshot.myBid = marketMaker.getMyBid();
    latestSnapshot.myAsk = marketMaker.getMyAsk();
    latestSnapshot.fairValue = marketMaker.getFairValue();

    latestSnapshot.hadFillThisTick = tickHadFill;
    latestSnapshot.lastFillPrice = tickLastFillPrice;
    latestSnapshot.lastFillQty = tickLastFillQty;

    latestSnapshot.inventory = marketMaker.getInventory();
    latestSnapshot.inventoryLimit = marketMaker.getMaxInventory();

    latestSnapshot.cash = marketMaker.getCash();

    latestSnapshot.realisedPnL = marketMaker.getRealisedPnL();
    latestSnapshot.unrealisedPnL = marketMaker.getUnrealisedPnL();
    latestSnapshot.totalPnL = marketMaker.getTotalPnL();
    latestSnapshot.spreadPnL = marketMaker.getSpreadPnL();
    latestSnapshot.inventoryPnL = marketMaker.getInventoryPnL();
    latestSnapshot.exposure = marketMaker.getExposure();
    latestSnapshot.adverseSelectionRatio = marketMaker.getAdverseSelectionRatio();
    latestSnapshot.fillCount = marketMaker.getTotalFillCount();
    latestSnapshot.buyFillCount = marketMaker.getBuyFillCount();
    latestSnapshot.sellFillCount = marketMaker.getSellFillCount();
    latestSnapshot.totalFilledVolume = marketMaker.getTotalFilledVolume();

    latestSnapshot.killSwitchActive = marketMaker.isKillSwitchActive();
}

const SimSnapshot& SimulationEngine::snapshot() const {
    return latestSnapshot;
}

bool SimulationEngine::isDataSourceReady() const {
    return config.source != SimConfig::Source::Historical || (replay && replay->isOpen());
}
