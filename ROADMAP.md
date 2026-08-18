# LOB-Sim — Roadmap

**Where it is now:** ~30KB of C++ across 11 translation units. Order book, matching engine
with price-time priority, EWMA fair value + realised vol, a market maker with inventory skew,
cost-basis PnL, kill switch and adverse-selection tracking, a regime-switching synthetic
trader, a pluggable `Strategy` interface, and a CSV replay of 2.11M real BTCUSDT trades.

That is a lot for three weeks of C++ from scratch. The core is genuinely sound. What's missing
is everything *around* the core: it can't be built by anyone else, it isn't tested, it produces
no data you can plot, and it only ever runs one strategy over 200 of the 2.11M available rows.

**The order of this roadmap matters.** Phases 0–2 are load-bearing: Phase 3 (plots) and Phase 7
(GUI) are both thin layers over the recorder built in Phase 2. Build the recorder once and both
come nearly free. Build the GUI first and you'll write the recorder twice.

Rough sizing: Phases 0–3 get you a project you can put on a CV with numbers in the bullet.
Phases 4–6 are what make it hold up under interview questioning. Phase 7 is the fun one.

---

## Phase 0 — Correctness and crash fixes

Do these first. Some are latent crashes; some are silently wrong numbers you'd end up plotting.

### 0.1 `HistoricalDataReplay::nextOrder()` will crash on the last line — **crash**

`hasNext()` is `file.peek() != EOF`. A file ending in a newline leaves one empty read: `getline`
returns `""`, `fields` ends up empty, and `fields[1]` is out-of-bounds on an empty vector —
undefined behaviour, not an exception. You haven't hit it because you stop at 200 rows.

```cpp
bool HistoricalDataReplay::nextOrder(Order& out) {   // return bool, don't return by value
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields = split(line, ',');
        if (fields.size() < 6) continue;             // malformed / header row
        if (!isdigit(static_cast<unsigned char>(fields[1][0]))) continue;  // header guard
        ...
        return true;
    }
    return false;
}
```

Also add a `try/catch` around `std::stod` or use `std::from_chars` — `stod` throws
`std::invalid_argument` on a header row and you currently have no handler.

### 0.2 Quantity scaling overflows `int` — **silent wrong numbers**

`static_cast<int>(rawQty * 1000000.0)` overflows for any trade above ~2147 BTC. More
immediately: your `BTTCUSDT-trades-2026-08-17.csv` file has quantities like `33848821.0`,
which scaled by 1e6 is 3.4e13 — catastrophic overflow into garbage.

Two fixes:

- Change `Order::quantity` to `int64_t`. One-line change, removes the whole class of problem.
- Better (see 4.1): store quantity in integer *lots* with an explicit lot size, and price in
  integer *ticks*. Then the scaling is a documented property of the instrument, not a magic
  `1000000.0` buried in a CSV parser.

### 0.3 That `BTTCUSDT` file is not Bitcoin

`BTTCUSDT-trades-2026-08-17.csv` has prices around `0.00000026`. That's BTTC — BitTorrent
Token. The typo in the filename went all the way through to downloading a genuinely different
asset. Delete it, or keep it deliberately as a second instrument to prove the sim isn't
hardcoded to BTC's scale (which is actually the better move — see 4.1).

### 0.4 PnL reconciliation warning uses an absolute tolerance

```cpp
if (std::abs(totalPnl - checkTotalPnl) > 1e-6)   // MarketMaker.cpp:126
```

Correct instinct — keeping two independent PnL calculations and asserting they agree is a
genuinely good habit and worth mentioning in an interview. But `1e-6` absolute against
BTC-scale values around 1e9 means floating-point noise trips it constantly. Use relative:

```cpp
double scale = std::max({1.0, std::abs(totalPnl), std::abs(checkTotalPnl)});
if (std::abs(totalPnl - checkTotalPnl) / scale > 1e-9) { ... }
```

### 0.5 `RandomTrader::generateOrder()` can produce negative prices

`bestBid()` returns `0.0` when the bid side is empty, so `price = bestBid - priceBuffer` goes
negative. Guard for a one-sided book and fall back to the last trade price.

Related: `bestBid()`/`bestAsk()` returning `0.0` as "no liquidity" is a sentinel that will bite
you again. Return `std::optional<double>` and let the compiler force you to handle the empty
case at each call site.

### 0.6 `getBestBid()` hands out a pointer into a vector that gets re-sorted

`addOrder()` calls `std::sort` on both vectors, invalidating any outstanding `Order*`. Your
comments in `MatchingEngine::process` show you've already been burned by this and worked around
it by ordering the bookkeeping carefully. That's a fragile invariant held together by a comment.
Phase 5.1 removes the hazard structurally.

### 0.7 `std::cout` in the hot path

Every trade, every quote, every cancel prints. At 200 rows it's noise; at 2.11M rows it *is*
the runtime — you'll be benchmarking your terminal, not your matching engine. Put all of it
behind a log level:

```cpp
enum class LogLevel { Silent, Summary, Trades, Debug };
```

Default `Silent` for batch runs. This is a prerequisite for Phase 5 benchmarking — without it
any performance number you quote is meaningless.

### 0.8 `main.cpp` never runs `FixedSpreadStrategy`

You wrote the baseline and never instantiate it. Right now "supports multiple strategies" is
true of the *interface* but not of any code path that executes. Fix in Phase 6, but be aware of
it now — don't put a claim on the CV that the repo doesn't back.

---

## Phase 1 — Make it buildable, testable, legible

Nobody can currently build this. `.vscode/tasks.json` hardcodes `C:\msys64\ucrt64\bin\g++.exe`
and lists every `.cpp` by hand. An engineer who clones your repo gets nothing.

### 1.1 CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(LOB_Sim CXX)
set(CMAKE_CXX_STANDARD 20)

add_library(lobsim_core
    src/Order.cpp src/OrderBook.cpp src/MatchingEngine.cpp
    src/MarketMaker.cpp src/FairValueModel.cpp src/RandomTrader.cpp
    src/DefaultStrategy.cpp src/FixedSpreadStrategy.cpp
    src/HistoricalDataReplay.cpp src/SimulationEngine.cpp)
target_include_directories(lobsim_core PUBLIC include)

add_executable(lobsim_cli src/main.cpp)
target_link_libraries(lobsim_cli PRIVATE lobsim_core)
```

**On the library split — it's smaller than it sounds, and it's optional.** `add_library` instead
of `add_executable`, plus one `target_link_libraries` line. You do not write an API document or
restructure headers; your existing `.h` files already are the interface. And because you are the
only consumer, "breaking the API" costs you one compiler error and one fix in the same commit —
none of the versioning/deprecation pain that makes real library design hard applies here.

The only reason to bother: you'll end up with three executables (CLI, tests, GUI). Without a
library you list the same ten `.cpp` files three times and compile them three times. If that
trade isn't worth it to you yet, list the files in each `add_executable` and move on — converting
later is a ten-minute change.

CMake itself is not optional, though. `tasks.json` hardcodes `C:\msys64\ucrt64\bin\g++.exe`, so
right now nobody else can build the repo at all.

While you're at it: `src/` and `include/` instead of 20 files in the repo root, and a
`.gitignore` for `*.exe` and the CSVs. `LOB-Sim.exe` is committed right now, and a 160MB CSV
should never go near git — use Git LFS or a `scripts/download_data.py`.

### 1.2 Tests

This is the single highest-value-per-hour item in the whole roadmap. A matching engine is
exactly the kind of component where "I wrote tests for it" is *expected*, and where their
absence is the first thing a reviewer notices. Catch2 or doctest via `FetchContent`, then:

**Order book**
- Price priority: better price fills first
- Time priority: same price, earlier `id` fills first
- Partial fill leaves correct residual quantity resting
- Cancel removes the right order and only that order
- Empty book on both sides

**Matching engine**
- Marketable buy walks multiple ask levels in order
- Non-marketable order rests instead of trading
- Aggressor gets the *resting* order's price, not its own
- Trade quantities sum to `min(aggressor qty, available liquidity)`

**Cost basis / PnL** (most valuable — this is where subtle bugs hide)
- Flat → long → flat round trip books the right realised PnL
- Adding to a position produces a size-weighted average cost
- Reducing a position leaves `avgCostBasis` unchanged
- **Flipping** long → short in one fill: realised PnL on the closed portion, new basis at the
  fill price for the remainder. Your `updateCostBasis` handles this; prove it.
- Realised + unrealised always equals `cash + inventory·mid − startingCash`

Aim for ~25 tests. That's an afternoon, and it converts "I built a matching engine" into "I
built a matching engine and can show you it's correct."

### 1.3 README

Short. What it is, how to build, one paragraph of architecture, the headline results table, and
the plots from Phase 3. This is what an engineer actually looks at — most people never open a
source file.

---

## Phase 2 — Make the simulation steppable and recorded

**This is the keystone phase.** Everything downstream depends on it.

Note this is a *control-flow* change, not a build or packaging one — it matters regardless of
whether you use CMake, a library, or a hand-written compile command.

### 2.1 Extract a `SimulationEngine`

Right now `runSimulation()` is a closed `for` loop that runs to completion and returns one
`double`. You cannot pause it, inspect it, or drive it from a UI. Invert the control flow:

```cpp
struct SimConfig {
    enum class Source { Synthetic, Historical } source;
    std::string dataPath;
    int maxSteps;
    unsigned seed;
    std::string strategyName;
    StrategyParams params;
    RiskLimits limits;
};

struct SimSnapshot {          // everything the recorder AND the GUI need
    int64_t step;
    int64_t timestampMicros;  // real time from the tape, 0 for synthetic
    double bestBid, bestAsk, mid, fairValue, volatility;
    double myBid, myAsk;
    int64_t inventory;
    double cash, realisedPnL, unrealisedPnL, totalPnL, exposure;
    int64_t fillCount;
    double adverseSelectionRatio;
    bool killSwitchActive;
    std::vector<PriceLevel> bidLevels, askLevels;  // top N, for the GUI ladder
};

class SimulationEngine {
public:
    explicit SimulationEngine(SimConfig cfg);
    bool step();                       // one round; false when the source is exhausted
    void reset();
    SimSnapshot snapshot() const;
    const RunSummary& summary() const;
};
```

Then `main.cpp` becomes `while (engine.step()) recorder.record(engine.snapshot());` and the GUI
in Phase 7 becomes `if (running) engine.step();` inside the render loop. Same engine, two
drivers, zero duplicated logic.

### 2.2 The recorder

Two output files per run:

**`run_<id>_ticks.csv`** — one row per step:
```
step,timestamp,best_bid,best_ask,mid,fair_value,volatility,my_bid,my_ask,
inventory,cash,realised_pnl,unrealised_pnl,total_pnl,exposure,filled,fill_side,fill_price,fill_qty
```

**`run_<id>_summary.json`** — one object per run: config echoed back, plus final PnL, Sharpe,
max drawdown, fill count, fill rate, adverse selection ratio, mean/max absolute inventory,
time-weighted inventory, kill-switch trips, wall-clock runtime, steps/sec.

Echoing the config into the summary matters more than it sounds — it's what lets Phase 6's
parameter sweep produce a single tidy dataframe you can `groupby` without bookkeeping.

Buffer writes and flush every few thousand rows; don't `std::endl` per row (it forces a flush
and will dominate your runtime).

### 2.3 Metrics worth computing

You already track adverse selection, which is the most interesting number in the project and
you currently never print it. Add:

- **Sharpe**, on per-step PnL deltas, scaled by steps-per-day from the tape timestamps
- **Max drawdown** on the cumulative PnL curve
- **Fill rate** — fills ÷ quotes posted. Tells you whether you're actually competitive
- **Spread capture** — realised PnL per fill, in ticks. The core market-making number
- **Time-weighted absolute inventory** — how much risk you carried, not just where you ended
- **Mark-out PnL** (see 4.4) — the professional version of adverse selection

---

## Phase 3 — Plotting

With Phase 2's CSV this is a short Python script, `analysis/plot_run.py`, using pandas +
matplotlib. One function per figure, saved to `analysis/figures/`:

1. **PnL decomposition** — realised, unrealised, total, on one time axis
2. **Inventory over time**, with `±maxInventory` bands shaded and kill-switch trips marked
3. **Quotes vs market** — mid, fair value, your bid and ask as a band, fills as scatter
   (green = bought, red = sold). This is the money plot: you can *see* the strategy working
4. **PnL distribution** across the 100 seeds — histogram with mean and ±1σ
5. **Drawdown curve**
6. **Mark-out curve** — average PnL at 1s / 5s / 30s after a fill (Phase 4.4). If it slopes
   down you're being picked off, and being able to say that sentence in an interview is worth
   more than the rest of the project combined
7. **Strategy comparison** — same axes, one line per strategy (Phase 6)

Keep the plots plain: no gridlines fighting the data, one accent colour, direct labels rather
than legends where you can. These end up in the README and they are the first thing anyone sees.

Add `analysis/requirements.txt` — pandas, matplotlib, numpy.

---

## Phase 4 — Realism

This is the phase that separates "a student built a simulator" from "this person understands
market microstructure." Each item here is an interview talking point.

### 4.1 Integer ticks and lots

Prices are `double` and you compare them with `<` and `!=` in the sort comparator and matching
loop. Real exchanges use integer ticks precisely because float comparison at a price level is
unreliable. Introduce:

```cpp
struct Instrument {
    std::string symbol;
    int64_t tickSize;    // in price units of 1e-8, say
    int64_t lotSize;
    double  toPrice(int64_t ticks) const;
};
```

Store `int64_t priceTicks` and `int64_t qtyLots` on `Order`. Convert at the I/O boundary only.
This kills the BTC-vs-synthetic scale mismatch you flagged in `main.cpp`, kills the `1000000.0`
magic number, kills float comparison, and lets you run BTC and BTTC through the same engine
unmodified — which is a much better demo than either alone.

### 4.2 Post-only quoting

Right now `quote()` routes through `matchingEngine.process()`, so when inventory skew pushes
your bid above the best ask, **you cross the spread and take liquidity**. Your comment says
this is deliberate, and it's a defensible choice — but a market maker that pays the spread when
it most wants to reduce risk is modelling something quite different from market making.

Add a `postOnly` flag: if the quote would cross, either clamp it to one tick inside the touch
or skip that side entirely. Then run both and compare. "I implemented both and post-only
improved PnL by X% while cutting fill rate by Y%" is a far better answer than either default.

### 4.3 Queue position

Currently you're always at the front of the queue and always fill in full. This is the single
biggest source of optimism in your historical results. A simple model gets most of the way:

- On posting at a price level, record the volume already resting there (`queueAhead`)
- Incoming trades at that level decrement `queueAhead` first
- You only fill once `queueAhead` hits zero
- Re-quoting at the same price keeps your position; moving price resets it to the back

Binance trade data doesn't give you book depth, so you'll estimate `queueAhead` from recent
volume at that level. Approximate — but state the assumption in the README and it's a strength,
not a weakness. Interviewers care much more that you *know* queue position matters.

### 4.4 Mark-outs

Replace (or supplement) the current adverse-selection proxy, which is lagged by one trade and
only resolves when the next trade happens. The standard measure: for each fill, record the mid
at t+1s, t+5s, t+30s, and compute signed PnL against the fill price.

```cpp
struct Fill { int64_t ts; double price; int64_t qty; OrderSide side; };
// later: markout(h) = side_sign * (midAt(fill.ts + h) - fill.price) * qty
```

Aggregate into a mark-out curve. A downward-sloping curve means informed flow is running you
over. This is *the* diagnostic market makers actually use.

### 4.5 Latency

Add a configurable delay between decision and order arrival — your quote at step *t* enters the
book at step *t + k*. Even a fixed one-step delay changes results materially and shows you know
that reacting to a price you've already seen is not free.

### 4.6 Don't quote off your own quotes

`calcMidPrice()` reads the book, but during historical replay the book is mostly *your own
resting orders* — so your fair value is partly a function of your own quotes. A feedback loop.
In replay mode, drive the reference price from the trade tape only, and keep the book mid for
execution. Worth a comment in the code explaining why the two differ.

---

## Phase 5 — Performance

You have 2.11M real trades and you're running 200 of them. Fix that, then make it fast, then
quote a number.

### 5.1 Replace the sorted vectors

`OrderBook::addOrder` calls `std::sort` on **both** sides on **every** insertion — O(n log n)
per order when only one side changed, and `removeBestBid()` does `erase(begin())`, which is
O(n). The standard structure:

```cpp
std::map<int64_t, std::deque<Order>, std::greater<>> bids;  // price ticks -> FIFO queue
std::map<int64_t, std::deque<Order>>                 asks;
std::unordered_map<int64_t, OrderLocation>           index; // order id -> O(1) cancel
```

Best bid/ask is `begin()`, insertion is O(log levels), cancel is O(1) via the index, and no
pointer invalidation — which retires issue 0.6 permanently.

### 5.2 Benchmark it

Add `benchmarks/bench_book.cpp`. Measure orders/sec through the matching engine before and
after 5.1, with logging off. Put both numbers in the README.

*"Replaced the vector-and-sort book with price-level maps plus an O(1) cancel index; matching
throughput went from X to Y orders/sec on 2.1M BTCUSDT trades"* is a CV bullet that gets you
asked about it in an interview, which is exactly what you want.

### 5.3 Run the whole file

Once logging is off and the book is O(log n), all 2.11M trades should run in seconds. Then your
results are a full trading day of real data instead of the first 90 seconds.

---

## Phase 6 — Strategies and sweeps

### 6.1 Actually run more than one

`main.cpp` should take arguments (`--strategy`, `--data`, `--steps`, `--seed`, `--out`) and run
whichever it's told. Add a `StrategyFactory` keyed by name so adding a strategy doesn't mean
touching `main`.

### 6.2 More strategies

- `FixedSpreadStrategy` — already written, currently dead code. Your baseline
- `DefaultStrategy` — skew + vol-scaled spread. Your current default
- **`AvellanedaStoikov`** — the canonical academic market-making model. Reservation price
  `s − q·γ·σ²·(T−t)` and optimal spread `γσ²(T−t) + (2/γ)ln(1 + γ/κ)`. Implementing it from
  the paper is a strong signal, and it makes your existing `DefaultStrategy` legible as a
  simplification of it
- **`AdaptiveSpread`** — widen on high realised vol or high inventory, tighten when flat

### 6.3 Parameter sweeps

Loop over a grid (`skewCoefficient` × `baseSpread` × `maxInventory`), N seeds each, one summary
row per run. Then a heatmap of Sharpe over the parameter grid. Two things fall out: you find
the good region, and you find out whether the good region is a broad plateau or a narrow spike
— i.e. whether you've found an edge or overfit a seed. Being able to discuss *that* distinction
is worth real credit.

Parallelise with `std::thread` over runs — trivially parallel, and it's another honest
concurrency talking point.

---

## Phase 7 — The GUI

Now it's earned. By this point `SimulationEngine::step()` and `snapshot()` already exist, so
the GUI is genuinely a renderer over an API you already have — a weekend, not a rewrite.

**Design it early, build it late.** Sketching the layout before Phase 2 is genuinely useful, not
procrastination: the panels you choose determine what `SimSnapshot` has to carry. A depth ladder
means snapshot needs top-N price levels; a mark-out panel means the engine must retain fill
history. Design the UI, then build the engine to feed it. What you should *not* do is implement
the GUI first — you'd spend a weekend building panels that display hardcoded zeros.

### 7.1 Stack: Dear ImGui + ImPlot + GLFW

- Immediate mode, so no widget state to synchronise with sim state — you draw straight from
  `snapshot()` each frame. Retained-mode toolkits (Qt) would have you writing model classes
- **ImPlot** gives you real-time scrolling plots essentially for free, which is most of what
  you want
- Vendored via CMake `FetchContent`, no system install, still builds on a clean clone
- Ships as a second CMake target (`lobsim_gui`) linking the same `lobsim_core`. The CLI keeps
  working, and the GUI stays optional behind `-DBUILD_GUI=ON`

Avoid Qt (heavyweight, licensing awkwardness, slow to learn) and avoid a web frontend (you'd be
writing a server, and the C++ stops being the interesting part).

### 7.2 Layout

Roughly what you described, arranged into four docked panels:

**Controls (top bar)**
Run / Pause / Step / Reset. Speed slider (steps per frame, 1 → 10,000). Data source toggle
(synthetic / historical). Strategy dropdown. Seed input. Live-editable strategy parameters —
`skewCoefficient`, `baseSpread`, `maxInventory`. Being able to drag a parameter and watch
inventory behaviour change *while it runs* is the single best reason to build this, and it's
a real research tool, not decoration.

**Order book ladder (left)**
Top ~10 levels each side, size bars, your own resting bid and ask highlighted in a distinct
colour so you can see where you sit relative to the touch. This is the "snapshot of what the
market maker is doing" you asked for.

**Plots (centre, ImPlot, scrolling window of the last N steps)**
- Price panel: mid, fair value, your bid/ask as a shaded band, fills as scatter markers
- Inventory panel: inventory over time with `±maxInventory` limit lines
- PnL panel: realised / unrealised / total

**Status (right)**
Live readout of `printStatus()` — inventory, cost basis, realised, unrealised, total, exposure,
fill count, adverse selection ratio, kill-switch state (red when tripped, with a Reset button).

### 7.3 Implementation notes

- Keep the sim on the render thread initially and run `speed` steps per frame — 60fps × 1000
  steps is 60k steps/sec, plenty. Only move to a worker thread with a double-buffered snapshot
  if you actually need more
- Ring buffers (`std::vector` of fixed capacity with a write cursor) for plot history, not
  unbounded growth. ImPlot reads them directly with a stride, no copying
- Add a "Run to end and export" button that runs headless at full speed and writes the Phase 2
  CSVs — this keeps the GUI and the batch path unified rather than divergent
- **Record a GIF** of it running and put it at the top of the README. This is where the "looks
  cool" value actually pays off. Nobody clones your repo; everybody scrolls a README

---

## Phase 8 — Packaging it for the CV

The work above is worthless on an application if it isn't legible in ten seconds.

- **README**: GIF at the top, one-paragraph description, build instructions, architecture
  diagram, results table, the plots, and an explicit "Assumptions and limitations" section.
  That last section reads as maturity, not weakness — every real backtest has one
- **CV bullet**: C++ isn't currently on your skills list at all. Add it, and write the bullet
  around numbers, not features:

  > *Built a limit order book simulator and market-making engine in C++20 (matching engine with
  > price-time priority, queue-position and latency modelling, pluggable strategies). Backtested
  > inventory-skewed quoting against 2.1M BTCUSDT trades; compared to a fixed-spread baseline it
  > [changed PnL variance by X%] at [Y] fill rate. Profiled and rebuilt the book with
  > price-level maps for [Z]× matching throughput.*

  Fill in X, Y, Z from Phase 3 and 5. Numbers get you the follow-up question; features don't.
- Note "GUI" nowhere on the CV. Let the README GIF do that job

---

## Suggested order of attack

| Order | Phase | Effort | Why now |
|---|---|---|---|
| 1 | 0 — bug fixes | ~half a day | Everything downstream inherits these numbers |
| 2 | 1.1 CMake + library split | ~2 hours | Unblocks tests, benchmark and GUI |
| 3 | 2 — engine + recorder | ~1 day | The keystone; unblocks plots and GUI both |
| 4 | 3 — plotting | ~half a day | First visible payoff; README material |
| 5 | 1.2 tests | ~1 afternoon | Highest credibility per hour spent |
| 6 | 5 — performance | ~1 day | Produces a quotable number |
| 7 | 6 — strategies + sweeps | ~1–2 days | Makes "tests multiple strategies" actually true |
| 8 | 4 — realism | ~2–3 days | The interview-differentiating phase |
| 9 | 7 — GUI | ~1 weekend | Now cheap, because Phase 2 did the hard part |
| 10 | 8 — packaging | ~half a day | Converts work into interviews |

If you only do three things: **Phase 0, Phase 2, Phase 3.** That gets you from "I wrote some
C++" to a project with charts and numbers behind it.
