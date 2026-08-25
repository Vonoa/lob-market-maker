# LOB-Sim — Desk Review

*A review of the repo as a senior market maker would read it: what makes me want to interview
the candidate, what makes me sceptical, and what to build next. Written August 2026 against
the state of the repo at that date.*

---

## The one-paragraph verdict

This is well above the median "I built an order book" project, and the reason is specific:
most candidate sims report a P&L number and stop. This one asks whether the P&L was real —
the spread-vs-inventory attribution, the cash reconciliation invariant, the separate
`judgedFillCount` denominator, the `referencePrice()` unification. That is risk-and-measurement
instinct, and it is the rarer half of the job. What holds it back is equally specific: **there
is no clock.** A tick is an event, not a moment. Every serious next step you want — latency,
Avellaneda-Stoikov, markouts, annualised Sharpe, queue timing — is blocked behind that one
gap. And because there are no fees, no queue position and no latency, the fill rate is
optimistic by an unknown but large factor, so nobody on a desk will believe the absolute P&L.

Fix the clock, widen the strategy interface, then add the models. In that order.

---

## Part 1 — File by file

### `core/Order.h` / `Order.cpp`

**Good.** Clean, minimal, `int64_t` quantity (correct instinct — never float quantities).
ID doubling as arrival sequence for FIFO tie-break is a legitimate simplification and is
documented as such.

**What a desk flags:**

- **`double price` used as a `std::map` key.** Floating-point equality on price levels. It
  works today because every price originates from the same parse or the same arithmetic, but
  it is the kind of thing that breaks silently the first time a price is computed two
  different ways. Real books use integer ticks: `int64_t priceTicks` plus a `tickSize`.
  That also fixes `cancelOrder()`'s `sideMap.find(loc.price)`, which is currently an exact
  float lookup that *must* hit.
- **No timestamp.** The single most consequential omission in the repo. The Binance CSV has
  a `time` column and `HistoricalDataReplay` throws it away. See Part 2.
- **`static int nextId` inside `createOrder()`.** Global mutable state, not thread-safe, and
  shared across every `SimulationEngine` in the process. That blocks parallel Monte Carlo and
  parameter sweeps — which is exactly what Phase 6 wants. Move the counter into an
  `OrderIdGenerator` owned by the engine.
- **`int id`, not `int64_t`.** 2.11M rows per file × multiple runs per process. 2^31 is
  reachable in a large sweep.
- **No order type, TIF, or owner.** No market/IOC/post-only, no participant ID. The absence
  of an owner ID is why self-trade detection has to be done by ID comparison from outside the
  book.

### `core/OrderBook.{h,cpp}`

**Good.** The map-of-deques structure is the right call and — more importantly — the comment
explaining *why* it beats sort-on-insert is the kind of reasoning I want to see. Aggregated
`BookLevel` output with an explicit note about why it doesn't distinguish ownership is good
API thinking. The `index` for O(1) cancel lookup is right.

**What a desk flags:**

- **`getBestBid()` returns `Order*` into a deque.** A pointer into a container that the
  callback chain can mutate. `MatchingEngine` has a careful comment about ordering to avoid
  invalidating it — which is honest, but the API is the defect, not the caller. Either return
  a copy plus an ID, or move the fill logic into the book (`book.fillBest(qty)`).
- **`0.0` as the "no bid" sentinel.** Look at how many places in this codebase now defend
  against it: `RandomTrader::generateOrder()`, `MarketMaker::calcMidPrice()`,
  `getUnrealisedPnL()`, `getExposure()`, `SimulationEngine::step()`'s kill-switch rationale.
  When a codebase keeps writing guards against its own sentinel, the sentinel is wrong.
  `std::optional<double>` costs nothing and deletes five guards.
- **No queue position.** Correctly listed in the README as the biggest source of optimism.
  The fix is cheaper than you think — see Part 2.

### `core/MatchingEngine.{h,cpp}`

**Good.** Correct price-time walk with partial fills, a clean `onTrade` callback seam, and
the comment about finishing bookkeeping before notifying listeners shows you thought about
reentrancy. That is a real production concern and most candidates never hit it.

**What a desk flags:**

- **`std::vector<Trade> trades` grows unbounded.** Nothing in the sim path reads it, and a
  full-file replay accumulates ~2M `Trade` structs for nothing. It will show up the moment
  you do serious performance work.
- **`Trade` has no timestamp and no aggressor flag.** Both are needed for markouts and for
  any honest adverse-selection measure. The aggressor side is knowable at match time — record it.
- **No self-trade prevention at the engine level.** `MarketMaker` detects self-trades after
  the fact and counts them. Real venues implement STP (cancel-newest / cancel-oldest /
  decrement-and-cancel). Implementing one of those, with the choice named, reads as exchange
  knowledge.
- **No fees or rebates.** This is the biggest single economic omission after queue position.
  Maker rebate versus taker fee *is* the market-making business. A sim that computes spread
  P&L at zero fees is measuring a market that doesn't exist. `makerFeeBps` / `takerFeeBps` in
  config, booked into cash on every fill, changes conclusions — sometimes the sign of them.

### `core/FairValueModel.{h,cpp}`

This is where a quant reviewer pushes hardest, because the name promises more than the code does.

- **`alpha = 0.8` on every trade print is effectively "last price".** Half-life is under half
  an observation. This isn't a fair value model, it's a lightly smoothed last trade. That's
  fine as a v1 — but it means `referencePrice()` in Historical mode is basically the last
  print, and the adverse-selection judgement (which compares a fill against the next EWMA
  value) is a one-print markout.
- **Volatility is per *event*, not per unit time.** Stdev of 20 consecutive price differences.
  Without timestamps you cannot convert this to a per-second σ — and *every* model you want to
  add (AS above all) needs σ per unit time.
- **No microprice.** You already have `BookLevel` with sizes. Size-weighted mid —
  `(bidSize·ask + askSize·bid) / (bidSize + askSize)` — is about ten lines, and order-book
  imbalance is the most reliably predictive short-horizon signal there is. This is the highest
  value-per-line change available to you anywhere in the repo.
- **Scale-dependent constants.** `DefaultStrategy` uses `baseSpread = 0.2` and
  `volatilityMultiplier = 1.0` for both a ~$100 synthetic instrument and BTC at ~$62,900.
  At synthetic scale the 0.2 floor dominates; at BTC scale the vol term does. Those are two
  completely different strategies wearing one name. Express spreads in basis points, or make
  them per-instrument config.

### `core/MarketMaker.{h,cpp}` — the strongest and the weakest file

**The strongest parts of the whole repo are here:**

- **Cost-basis accounting with the long→short flip handled and tested.** That case is wrong in
  most candidate projects and you both handled it and wrote the test for it.
- **The `getTotalPnL()` reconciliation check** — comparing realised+unrealised against
  cash + inventory·mark, with a *relative* tolerance and a comment explaining why absolute
  failed at BTC magnitudes. That is a risk-systems invariant. This is the single detail in the
  repo I'd bring up in an interview.
- **The `judgedFillCount` reasoning.** Refusing to divide a judged numerator by an unjudged
  denominator is exactly right, and the header comment explaining it is better than what I see
  in a lot of production code.
- **`referencePrice()` as the one place marks and quotes agree.** Correct, and the reasoning
  about stale replay liquidity manufacturing fake P&L is genuinely good analysis.
- **Inventory headroom clamping** rather than just suppressing a side. Makes `maxInventory`
  an actual limit rather than a trigger.

**And the structural weakness:** this one class is doing four jobs that are four separate
systems on a desk — quoting, order management, position/P&L keeping, and risk. 435 lines,
~30 members. Split it:

```
Position      inventory, avgCostBasis, realised/unrealised, cash, reconciliation
RiskLimits    maxInventory, maxLoss, maxExposure, kill switch
Quoter        order lifecycle: what's resting, what to cancel, what to send
Strategy      pricing only (already separate — good)
```

That split is what makes multi-strategy, multi-instrument, and per-strategy risk possible
later. It is also the thing that makes this repo look like it was designed rather than grown.

**Other flags:**

- **Cancel-and-replace on every single tick, unconditionally.** Two problems. On a real venue
  that message rate gets you rate-limited or fined. And it throws away queue priority every
  tick — which is free today because queue position isn't modelled, and catastrophic the
  moment it is. Real quoters re-price only when the desired price moves beyond a threshold.
  Add `requoteThreshold`; skip the cancel when the new price equals the resting one.
- **One quote per side, one price, always full size.** No laddering, no size decisions.
- **`startingCash = 100000.0` hardcoded** while Historical mode runs exposure in the billions.
  The number is meaningless there, and the reconciliation is running at ~1e9 magnitudes as a
  result. Make it config.
- **Adverse selection is a one-print markout.** The metric a desk actually looks at is a
  *markout curve*: signed P&L of each fill measured at fixed horizons after it — 100ms, 1s,
  10s, 1min. That single chart tells you whether you're being picked off and at what timescale.
  Blocked on timestamps.

### `strategies/` — the interface is the bottleneck

`Strategy::computeQuote(mid, inventory, volatility, spread) const` is clean and the two
implementations are honest baselines with well-documented empirical tuning. The skew-clamp
reasoning (with the sweep numbers in the comment) is good work.

**But this signature cannot express any of the models you want to add.** Specifically:

| Missing | Blocks |
|---|---|
| Time remaining `T−t` | Avellaneda-Stoikov — the reservation price *is* `s − qγσ²(T−t)` |
| Risk aversion γ, arrival intensity κ | AS optimal spread |
| Book state (imbalance, depth) | Any microstructure signal |
| Per-strategy mutable state (`const`!) | Glosten-Milgrom is a Bayesian belief update — inherently stateful |
| Fill / trade callbacks | κ estimation, GM updating, any online learning |
| Size in the return value | Size is half of a quoting decision |
| Ability to decline one side | Any risk-aware quoter |

Widen it **before** writing AS or GM, not after — otherwise you'll bolt both models onto a
signature that can't hold them and the result will look like it. Proposed shape:

```cpp
struct MarketState {
    int64_t timestampNs;
    double  mid, microprice, fairValue;
    double  bookSpread;
    double  volatility;        // per unit TIME, not per event
    std::vector<BookLevel> bidLevels, askLevels;
    int64_t inventory, inventoryLimit;
    double  timeRemaining;     // (T - t), normalised 1.0 -> 0.0
};

struct QuoteSide { double price; int64_t size; bool active; };
struct Quotes    { QuoteSide bid, ask; };

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual Quotes computeQuotes(const MarketState&) = 0;  // NOT const — strategies estimate
    virtual void   onFill(const Fill&)   {}                // κ estimation, GM belief update
    virtual void   onTrade(const Trade&) {}                // observe flow you weren't part of
    virtual void   reset()               {}
};
```

### `simulation/SimulationEngine.{h,cpp}`

**Good.** `step()`/`snapshot()` is the right shape, and the decision to hand out a *copy* of
state rather than references into engine internals is the correct call for a GUI consumer.
The comment explaining why the two order sources have deliberately different shapes (quiet
tick vs. exhausted feed) is exactly the sort of thing that stops a future refactor breaking it.

**What a desk flags:**

- **Tick = event; there is no clock.** Root cause of most of this document.
- **The book is seeded with two hardcoded orders that never get cancelled.** They live in the
  book for the whole run — that *is* the stale-liquidity problem you documented, partly
  self-inflicted.
- **In Historical mode, the "order book" is a fiction.** The feed is trades-only, nothing ever
  cancels, so the book is your own quotes plus an accumulating sediment of unfilled replay
  remainders. You document this honestly, and I'd go further and say it plainly in the README:
  *in Historical mode there is no book.* Three ways out, in increasing order of effort:
  1. **Fill model instead of a book** (what most production backtesters actually do): you're
     filled when a trade prints through your quote, for `min(tradeQty, yourSize)`, haircut by
     an assumed queue position. Honest, simple, and removes the fiction entirely.
  2. **Synthesise a book** around each trade print with an arrival/cancel decay model.
  3. **Use a real L2/depth or order-by-order feed.** Binance publishes both. Most faithful,
     most work, and the only one that lets you model queue position for real.
- **`isKillSwitchActive()` ends the run.** It conflates "risk halted" with "no more data". A
  real system stops quoting, flattens, and keeps marking — you want to see the P&L path after
  the halt, not truncate it.
- **No `reset()`** despite the roadmap naming it; re-running means reconstructing the engine.
- **Config lives in code.** A parameter sweep currently requires a recompile. JSON or CLI
  config is a precondition for Phase 6.

### `simulation/Recorder.{h,cpp}`

**Good.** One-pass streaming accumulators rather than re-reading the CSV, a JSON summary, and
— credit where it's due — explicitly labelling the Sharpe as *per-tick, not annualised*
instead of quietly reporting a flattering number. That honesty is worth more than the metric.

**Bugs and gaps:**

- **`variance = E[x²] − E[x]²` is numerically unstable at your magnitudes.** With P&L values
  around 1e9, `sumPnLDeltaSquared` and `mean*mean` are both enormous and nearly equal —
  catastrophic cancellation. You can get a negative variance out of this. Use **Welford's
  online algorithm**; it's the same one-pass shape and it's stable.
- **Default float formatting silently quantises the CSV.** `operator<<` gives six significant
  digits, so a P&L of `1234567891.23` is written as `1.23457e+09`. Your recorded P&L series is
  losing real precision at BTC scale. `file << std::setprecision(12)` or fixed formatting.
- **No per-fill log.** The CSV is per-tick *state*. What you actually need for MM diagnostics
  is `fills.csv`: timestamp, side, price, size, queue position at entry, mid at fill, and mid
  at +100ms / +1s / +10s / +60s. Almost every meaningful market-making chart comes from that
  one file, including the markout curve.
- **No drawdown duration**, only depth.

### `tests/`

Fifteen cases over book, matching, and cost basis, including the long→short flip. Right
instincts, right target.

**Gaps that are cheap to close and disproportionately persuasive:**

- Nothing tests the kill switch, the P&L reconciliation invariant, or the adverse-selection
  counting — i.e. exactly the logic you're proudest of.
- Nothing tests `HistoricalDataReplay` parsing. That parser has real edge cases: it validates
  `fields[1][0]` is a digit, so a leading space or a negative sign silently skips a row.
- **No property-based test on the matching engine.** For random order sequences, assert:
  total traded quantity balances across sides; book invariants hold (best bid < best ask, no
  empty levels, `index` size equals total resting orders); conservation of quantity. Cheap to
  write, and it's the test that convinces someone your engine is actually correct.
- No CI. A GitHub Actions workflow running `lobsim_tests` on push is ~20 lines and turns
  "15 unit tests" from a claim into a badge.
- No sanitiser build. Given a codebase that hands out `Order*` into deques, an
  `-fsanitize=address,undefined` run coming back clean is a strong statement to be able to make.

### Repo hygiene

`LOB-Sim.exe`, `TestSimEngine.exe`, and six `run_*.csv` / `run_*_summary.json` files are
committed — and the run outputs are duplicated in both the repo root and `gui/`. Reviewers
notice. `.gitignore` them.

---

## Part 2 — The four things that unblock everything

### 1. Timestamps, end to end (do this first)

Add `int64_t timestampNs` to `Order`, `Trade`, and `SimSnapshot`. Parse the `time` column you're
already discarding in `HistoricalDataReplay`. Give `RandomTrader` a synthetic clock (exponential
inter-arrival times — see Hawkes below for the better version).

This one change unblocks: latency, markouts, per-second volatility, annualised Sharpe,
time-weighted inventory, AS's `T−t`, and queue timing. Nothing else on this list is worth
starting first.

### 2. Queue position (biggest realism win per line of code)

Right now a resting order is assumed to be at the front of the queue and fills in full the
instant anything marketable arrives. That is the single largest source of optimism in your fill
rate. The `std::deque` already models the queue — you just aren't reading it.

When you insert an order, record `volumeAhead` = total resting quantity already at that price.
When a trade executes at that price, decrement `volumeAhead` first; only fill you once it hits
zero. Expose `queuePosition` in the snapshot (the GUI mockup already wanted it).

Then run the experiment: **fill rate and P&L under front-of-queue vs. realistic queue.** The
gap between those two numbers is the honest measure of how much your current results are
overstating things, and reporting it is far more impressive than hiding it.

### 3. Fees and rebates

`makerFeeBps`, `takerFeeBps` in `SimConfig`; book them into cash on every fill, tagged by
whether you were maker or taker. Market making is a rebate business. A strategy that's
profitable at zero fees and unprofitable at realistic ones is the normal case, and knowing
which side of that line you're on is the point.

### 4. Latency — and why it needs an event queue

This is the item you asked about, and it's the one that turns this from a toy into a simulator.
It cannot be bolted onto the current tick loop, because latency is fundamentally about
*ordering in time*, and a tick loop has no time.

**The design.** Replace the tick loop with a timestamp-ordered event queue:

```cpp
struct Event {
    int64_t ts;
    enum class Kind { MarketData, OrderArrives, CancelArrives, StrategyWake } kind;
    // payload
    bool operator>(const Event& o) const { return ts > o.ts; }   // min-heap by ts
};
std::priority_queue<Event, std::vector<Event>, std::greater<>> events;
```

The loop pops the earliest event, not the next row. Then apply three separate latencies:

- **Market data in (`latencyIn`)** — the strategy sees the world as it was `latencyIn` ago.
  Every decision is made on a stale view.
- **Order out (`latencyOut`)** — a quote decided at `t` reaches the book at `t + latencyOut`.
- **Cancel out (`latencyCancel`)** — and *this is where the pain lives*. Your cancel is in
  flight while the market runs through your stale quote. You get filled on a price you already
  decided to leave. **That is adverse selection, mechanically.** Without cancel latency, your
  adverse-selection ratio is measuring almost nothing.

Model each as a distribution, not a constant: `base + jitter`, with jitter lognormal or gamma,
plus rare spikes. Constant latency is unrealistically kind — it's the tail that hurts.

**The experiment worth running.** Sweep latency from 0 → 10ms and plot P&L, fill rate, and
adverse-selection ratio against it. The shape of that curve is a real market-making truth, it's
your own result rather than a formula from a paper, and it is a much better thing to talk about
in an interview than any feature list.

---

## Part 3 — The models you asked about

### Avellaneda-Stoikov (2008)

**The model.** Optimal quotes for an inventory-averse MM over a finite horizon:

```
reservation price   r = s − q · γ · σ² · (T − t)
optimal half-spread δ = ½ γ σ² (T − t) + (1/γ) · ln(1 + γ/κ)
```

where `q` is inventory, `γ` risk aversion, `σ` volatility **per unit time**, `T−t` time to
horizon, and `κ` the decay of fill intensity with distance from mid (`λ(δ) = A·e^(−κδ)`).

**What you need that you don't have:** σ per unit time (→ timestamps), `T−t` (→ a session
horizon in the interface), and `κ`.

**`κ` is the interesting part, and it's where the real work is.** It must be *estimated*, not
guessed: log every quote's distance from mid and whether it filled, bucket by distance, fit an
exponential to the resulting fill-rate curve. You already log fills; this is a natural use of
the `onFill` hook. Estimating κ from your own tape is the difference between a candidate who
implemented a formula and one who understands what the formula is made of.

**Be loud about the caveats in the README** — this is what separates having read the paper from
having understood it. AS assumes a Brownian mid unaffected by your own quoting, symmetric
Poisson arrivals, no queue, no fees, no adverse selection, and unlimited quote size. Your sim
violates every one. Also note the known artifact: the `(T−t)` terms drive the spread toward
zero as `t → T`, which is why practitioners use the infinite-horizon variant or floor it.

Best framing: *"AS as a baseline, then show where it breaks in this sim, and why."* That is a
far stronger interview answer than "I implemented AS."

### Glosten-Milgrom (1985) — a different kind of model

Worth saying explicitly, because it's easy to file both under "MM models" and they aren't:
**AS is a control problem about inventory; GM is an information problem about adverse
selection.** GM explains *why spreads exist at all* — the market maker must widen enough to
recoup, from uninformed flow, what informed flow takes. It's an equilibrium condition, not a
quoting algorithm you tune.

**Implementation shape.** Maintain a belief distribution over the true value `V`. Each incoming
trade is evidence: a buy raises `P(V high)`, a sell lowers it. Bayes-update on every trade —
including trades you weren't part of. Then quote:

```
ask = E[V | next order is a buy]
bid = E[V | next order is a sell]
```

with `α` = fraction of informed traders as the key parameter.

**This requires stateful strategies with an `onTrade` hook**, which the current `const`
interface makes impossible. It's the concrete reason to widen the interface first.

**Why GM fits *this* repo particularly well:** you already measure adverse selection. GM gives
you a theoretical prediction of what the spread *should* be given that adverse-selection rate.
Plotting your measured adverse selection against the GM-implied spread is a genuinely
sophisticated result and it's original to your data.

**Caveat:** pure GM has no inventory and no risk aversion — implemented alone it produces an
inventory-blind quoter that will happily accumulate an unbounded position. The interesting
version is **GM's information component driving fair value, feeding AS's inventory control**.
That hybrid is a legitimately good piece of work and I'd take it over either model alone.

### Other directions, ranked by value to this project

1. **Order-book imbalance / microprice** as both fair value and signal. Least academic, most
   directly connected to how short-horizon money is actually made, ~10 lines given `BookLevel`.
2. **Hawkes process order flow** for `RandomTrader`. Self-exciting arrivals produce clustered
   volatility and realistic bursts — much closer to real tape than a Bernoulli coin flip with
   three hardcoded regimes on a 20-round timer.
3. **Guéant-Lehalle-Fernandez-Tapia** — the practical closed-form AS variant with hard
   inventory bounds. Closer to what desks actually run than vanilla AS.
4. **Cartea-Jaimungal** — modern stochastic control with alpha signals; the natural next step
   once AS is in and you want to add a predictive signal to it.
5. **Almgren-Chriss** — only if you want to model the cost of *unwinding* accumulated
   inventory, which pairs naturally with a kill switch that flattens rather than just halting.

---

## Part 4 — How this reads as a hiring signal

**What makes me want the interview:**

- The P&L attribution work and, more than the work itself, the reasoning about why the naive
  version was wrong. Most candidates never ask whether their P&L came from edge or from drift.
- The reconciliation invariant with a relative tolerance and a comment explaining why absolute
  failed. That's a risk-systems reflex.
- The `judgedFillCount` denominator. Careful thinking about what a metric actually measures.
- The "Known limitations" section, and the AI-use disclosure. Being straight about what a model
  doesn't do is worth more than the model. **Keep both. Expand the limitations section.**
- Comments that explain *why* rather than *what*, throughout. Unusual, and it makes the code
  reviewable by someone who wasn't there.

**What makes me sceptical, in the order I'd raise it:**

1. **No clock.** I'd ask about this within two minutes and it weakens most of your metrics.
2. **No fees, no queue, no latency**, so the fill rate — and therefore every P&L number — is
   optimistic by an unknown factor. Stop leading with absolute P&L. Lead with *relative*
   comparisons and diagnostics, which are robust to exactly the assumptions you can't defend.
3. **In Historical mode there is no real book.** Say so bluntly in the README rather than
   leaving it implied in a design note.
4. **`MarketMaker` is four systems in one class**, and the `Strategy` interface can't express
   the models you're about to add.

**The reframe worth making.** The CV line isn't *"built a limit order book and market-making
simulator in C++"* — everyone writes that. It's:

> *Quantified how market-making edge decays with latency and queue position on real BTCUSDT
> tape.*

That's a result rather than a feature list, it's specific to work you did, and it invites
exactly the conversation you want to be having.

---

## Suggested order of work

| # | Work | Why now |
|---|---|---|
| 0 | Timestamps end-to-end (`Order`, `Trade`, `SimSnapshot`, `Recorder`, parse the CSV `time` column) | Unblocks literally everything below |
| 1 | Widen `Strategy` → `MarketState` / `Quotes`, non-`const`, `onFill` / `onTrade` hooks | Do this *before* AS/GM, not after |
| 2 | Queue position + fees + per-fill log with markouts | Biggest realism gain per line; makes results defensible |
| 3 | Event-queue engine with in/out/cancel latency distributions | Turns a toy into a simulator |
| 4 | Microprice + book imbalance in `FairValueModel` | Cheapest genuine alpha in the repo |
| 5 | Avellaneda-Stoikov with **estimated** κ, then Glosten-Milgrom with belief updating, then the hybrid | Now they have an interface that can hold them |
| 6 | The charts: edge vs latency, edge vs queue assumption, measured adverse selection vs GM-implied spread | These are the deliverable |

Housekeeping to do alongside, all cheap: Welford's variance in `Recorder`, CSV precision,
`std::optional` instead of the `0.0` sentinel, `int64_t` order IDs off a non-static generator,
`.gitignore` the committed binaries and run outputs, CI running the tests, one sanitiser build.
