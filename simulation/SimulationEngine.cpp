#include "SimulationEngine.h"

int64_t LatencyModel::sample(std::mt19937& rng) const {
    if (jitterNs <= 0.0) {
        return static_cast<int64_t>(baseNs); // no jitter configured - constant delay (0 by default)
    }
    std::exponential_distribution<double> jitter(1.0); // scaled by jitterNs below, same trick as RandomTrader's clock
    return static_cast<int64_t>(baseNs + jitterNs * jitter(rng));
}

// Member initializer list order follows DECLARATION order in the header
// (config, strategy, orderBook, matchingEngine, marketMaker, ...), not the
// order written here - orderBook and matchingEngine must already exist
// before marketMaker's constructor runs, since it takes references to both.
SimulationEngine::SimulationEngine(SimConfig config, Strategy& strategy)
    : config(config), strategy(strategy),
      orderBook(), matchingEngine(orderBook),
      marketMaker(orderBook, matchingEngine, strategy,
                   config.maxInventory, config.maxOrderSize,
                   config.maxLoss, config.maxExposure),
      // Distinct stream from randomTrader's own rng/clockRng - drawing latency
      // jitter from a shared generator would perturb every other draw's
      // position in that generator's sequence, the same bug Stage 1's
      // synthetic clock hit (see RandomTrader's clockRng comment).
      latencyRng(config.seed + 0x2545F4914F6CDD1Dull) {

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
    //
    // wasMyBid/wasMyAsk are captured BEFORE onTrade() runs, not after: onTrade()
    // doesn't touch currentBidId/currentAskId itself (only cancelQuotes()/
    // cancelSpecificOrder() do), so the comparison is valid either way, but
    // reading it explicitly here - rather than re-deriving "was this mine" from
    // trade.price later (a GUI price-match heuristic) - is real id-based
    // attribution, exposed on SimSnapshot as lastFillWasMine/lastFillMineSide.
    matchingEngine.setOnTrade([this](const Trade& trade) {
        bool wasMyBid = trade.buyOrderId == marketMaker.getCurrentBidId();
        bool wasMyAsk = trade.sellOrderId == marketMaker.getCurrentAskId();

        marketMaker.onTrade(trade);

        tickHadFill = true;
        tickLastFillPrice = trade.price;
        tickLastFillQty = trade.quantity;
        if (wasMyBid || wasMyAsk) {
            tickLastFillWasMine = true;
            tickLastFillMineSide = wasMyBid ? OrderSide::BUY : OrderSide::SELL;
        }
    });

    // Bootstrap the first event. Everything after this is self-sustaining -
    // each handler schedules whatever comes next (the next historical row,
    // the next synthetic round, or the next stage of an in-flight quote
    // decision), so nothing outside this constructor ever needs to seed the
    // queue again.
    if (config.source == SimConfig::Source::Synthetic) {
        bool willTrade = randomTrader->shouldTrade();
        SimEvent first;
        first.kind = SimEvent::Kind::MarketOrderArrives;
        first.ts = randomTrader->currentTimeNs();
        first.willTrade = willTrade;
        pushEvent(first);
    } else {
        scheduleNextHistoricalOrder();
    }
}

void SimulationEngine::pushEvent(SimEvent event) {
    event.seq = nextEventSeq++;
    events.push(std::move(event));
}

void SimulationEngine::scheduleNextHistoricalOrder() {
    Order nextOrder;
    if (replay && replay->nextOrder(nextOrder)) {
        SimEvent next;
        next.kind = SimEvent::Kind::MarketOrderArrives;
        next.ts = nextOrder.timestampNs;
        next.order = nextOrder;
        pushEvent(next);
    }
    // else: feed genuinely exhausted - nothing further to schedule. step()
    // ends the run once the whole queue (including any still-in-flight quote
    // decisions from the last row that DID arrive) has drained.
}

void SimulationEngine::scheduleRequote(int64_t triggerTs) {
    // Captured NOW, at the trigger's own timestamp - a nonzero latencyIn
    // means the strategy decides against this exact (increasingly stale)
    // snapshot later, not a freshly-rebuilt one. Building it unconditionally
    // (even if the kill switch is about to be checked and fail at wake time)
    // keeps the "check once, at decision time" guard in exactly one place -
    // MarketMaker::checkCanQuote(), called from handleStrategyWake().
    SimEvent wake;
    wake.kind = SimEvent::Kind::StrategyWake;
    wake.ts = triggerTs + config.latencyIn.sample(latencyRng);
    wake.marketState = marketMaker.buildMarketState(triggerTs);
    pushEvent(wake);
}

void SimulationEngine::handleMarketOrderArrives(const SimEvent& event) {
    if (config.source == SimConfig::Source::Historical) {
        const Order& order = event.order; // parsed at schedule time - the tape fixes price/qty/time, not the live book

        // incoming order's price is the raw historical exchange price, parsed straight
        // from the CSV before anything in this sim touches it - the one genuinely
        // external, non-self-referential price observation available each row. Recorded
        // before this round's requote is triggered so that decision reflects it. See
        // MarketMaker::onTrade()'s comment for why this has to replace (not just
        // supplement) feeding fairValueModel off trade.price in Historical mode.
        marketMaker.recordExternalPrice(order.price, order.timestampNs);

        matchingEngine.process(order); // matches against whatever is CURRENTLY resting

        scheduleRequote(event.ts);
        scheduleNextHistoricalOrder();
    } else {
        if (event.willTrade) {
            // Built lazily HERE, not at schedule time - generateOrder() prices itself
            // off the live book (bestBid/bestAsk), which may have changed since this
            // round was scheduled (e.g. an intervening QuoteArrives from a prior
            // round's decision). randomTrader's clock still reads this round's
            // timestamp (event.ts) at this point, since shouldTrade() below - which
            // advances it - hasn't run yet this round.
            Order order = randomTrader->generateOrder();
            matchingEngine.process(order);
        }

        // Quoted every round regardless of whether it traded - matches the original
        // synchronous loop, which called quote() unconditionally before deciding
        // whether a random order would even exist this tick.
        scheduleRequote(event.ts);

        // Advance to the NEXT round's timestamp/trade-or-not decision now, regardless
        // of whether THIS round traded - matches shouldTrade()'s "once per round"
        // contract from Stage 1.
        bool nextWillTrade = randomTrader->shouldTrade();
        SimEvent next;
        next.kind = SimEvent::Kind::MarketOrderArrives;
        next.ts = randomTrader->currentTimeNs();
        next.willTrade = nextWillTrade;
        pushEvent(next);
    }
}

void SimulationEngine::handleStrategyWake(const SimEvent& event) {
    if (!marketMaker.checkCanQuote()) {
        return; // dropped - no decision this cycle, exactly matching quote()'s own guard
    }

    Quotes decided = marketMaker.decideQuotes(event.marketState);

    // Cancel whatever's currently resting BEFORE the new quote is even decided
    // to post - captures the SPECIFIC order id now, since currentBidId/
    // currentAskId may have moved on by the time this cancel actually arrives.
    if (marketMaker.hasRestingBid()) {
        SimEvent cancelBid;
        cancelBid.kind = SimEvent::Kind::CancelArrives;
        cancelBid.ts = event.ts + config.latencyCancel.sample(latencyRng);
        cancelBid.cancelOrderId = marketMaker.getCurrentBidId();
        cancelBid.cancelSide = OrderSide::BUY;
        pushEvent(cancelBid);
    }
    if (marketMaker.hasRestingAsk()) {
        SimEvent cancelAsk;
        cancelAsk.kind = SimEvent::Kind::CancelArrives;
        cancelAsk.ts = event.ts + config.latencyCancel.sample(latencyRng);
        cancelAsk.cancelOrderId = marketMaker.getCurrentAskId();
        cancelAsk.cancelSide = OrderSide::SELL;
        pushEvent(cancelAsk);
    }

    SimEvent quoteArrives;
    quoteArrives.kind = SimEvent::Kind::QuoteArrives;
    quoteArrives.ts = event.ts + config.latencyOut.sample(latencyRng);
    quoteArrives.decidedQuotes = decided;
    quoteArrives.decisionMid = event.marketState.mid;
    pushEvent(quoteArrives);
}

void SimulationEngine::handleQuoteArrives(const SimEvent& event) {
    marketMaker.postBid(event.decidedQuotes.bid, config.quoteSize, event.decisionMid, event.ts);
    marketMaker.postAsk(event.decidedQuotes.ask, config.quoteSize, event.decisionMid, event.ts);
}

void SimulationEngine::handleCancelArrives(const SimEvent& event) {
    marketMaker.cancelSpecificOrder(event.cancelOrderId, event.cancelSide);
}

bool SimulationEngine::step() {
    // Run length is bounded by config.maxTicks (now an EVENT count - see the
    // class comment), matching the CLI loops this replaced. Returning false
    // is how the caller (CLI loop or GUI frame) is told the run is over.
    if (currentTick >= config.maxTicks) {
        return false;
    }

    // Once the kill switch trips, MarketMaker stops quoting for good (no
    // auto-reset), but the order source (RandomTrader/HistoricalDataReplay)
    // has no idea that happened and would keep feeding orders into a book
    // with no market maker to trade against. Ending the run here instead -
    // once halted, stay halted - is what a kill switch is supposed to mean.
    if (marketMaker.isKillSwitchActive()) {
        return false;
    }

    if (events.empty()) {
        // Genuinely nothing left: Historical's feed is exhausted AND every
        // in-flight decision from the last row that DID arrive has resolved.
        // (Synthetic never empties on its own - each MarketOrderArrives
        // reschedules the next round unconditionally.)
        return false;
    }

    SimEvent event = events.top();
    events.pop();

    if (config.maxSimTimeNs > 0 && event.ts > config.maxSimTimeNs) {
        return false; // past the configured time horizon - the run ends here, this event is dropped
    }

    // Reset BEFORE this event runs, not after - the onTrade callback (set up in
    // the constructor) can set these DURING handling, so resetting afterward
    // would immediately wipe out the very fill it just recorded.
    tickHadFill = false;
    tickLastFillPrice = 0.0;
    tickLastFillQty = 0;
    tickLastFillWasMine = false;

    switch (event.kind) {
        case SimEvent::Kind::MarketOrderArrives:
            handleMarketOrderArrives(event);
            break;
        case SimEvent::Kind::StrategyWake:
            handleStrategyWake(event);
            break;
        case SimEvent::Kind::QuoteArrives:
            handleQuoteArrives(event);
            break;
        case SimEvent::Kind::CancelArrives:
            handleCancelArrives(event);
            break;
    }

    lastEventTimestampNs = event.ts;
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
    latestSnapshot.timestampNs = lastEventTimestampNs;

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
    latestSnapshot.lastFillWasMine = tickLastFillWasMine;
    latestSnapshot.lastFillMineSide = tickLastFillMineSide;

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
