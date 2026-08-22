# LOB-Sim

A limit order book and market-making simulator written in C++17, with a real-time Dear ImGui /
ImPlot GUI. It runs a price-time-priority matching engine against either a synthetic
regime-switching order flow or a replay of real BTCUSDT trade data, and quotes into that book
with a pluggable market-making strategy — tracking inventory, cost-basis P&L, exposure, and
adverse selection as it goes.

## Features

- **Matching engine** — price-time priority, partial fills, self-trade detection, a map-based
  order book (`O(log levels)` insert, `O(1)` cancel via an id index).
- **Market maker** — inventory-skewed quoting with a clamped skew (so a quote can never cross
  mid), vol-scaled spread, a kill switch on loss/exposure limits, and full cost-basis P&L
  (realised vs. unrealised, spread P&L vs. inventory P&L).
- **Two data sources** — a synthetic regime-switching random trader for fast iteration, and a
  replay of real BTCUSDT trade data (CSV) for a real market's microstructure.
- **Two strategies out of the box** — `DefaultStrategy` (inventory skew + volatility-scaled
  spread) and `FixedSpreadStrategy` (a non-adaptive baseline to compare against). Both implement
  a small `Strategy` interface, so adding a new one doesn't touch the engine.
- **A steppable engine** — `SimulationEngine::step()`/`snapshot()` drives the CLI, the recorder,
  and the GUI off the exact same code path; nothing is duplicated between them.
- **A live GUI** — Order Book ladder with depth bars, My Quotes, Live Stats, Trade Tape, and a
  Market panel plotting mid / fair value / your quote band / your fills (buy vs. sell) on one
  axis, plus an Inventory panel with turnover tracking. See [`GUI_PANELS.md`](GUI_PANELS.md) for
  what each panel shows and why.
- **15 unit tests** (doctest) covering the order book, matching engine, and cost-basis P&L —
  including the trickiest case, flipping a position from long to short in a single fill.

## Architecture

```
core/         Order, OrderBook, MatchingEngine, MarketMaker, FairValueModel, Logging
strategies/   Strategy (interface), DefaultStrategy, FixedSpreadStrategy
simulation/   SimulationEngine (step/snapshot), RandomTrader, HistoricalDataReplay, Recorder
gui/          main_gui.cpp — Dear ImGui + ImPlot + GLFW front end
tests/        doctest unit tests
main.cpp      CLI entry point — Monte Carlo over Synthetic, plus Historical replay runs
```

`core/`, `strategies/`, and `simulation/` are plain include directories, not a compiled library —
every file keeps a flat `#include "OrderBook.h"` regardless of which folder it lives in. Three
targets (`lobsim`, `lobsim_tests`, `lobsim_gui`) compile the same sources independently rather
than sharing a library, since duplicating a handful of `.cpp` files across `add_executable()`
calls is simpler than versioning an internal API only this repo ever consumes.

## Building

Requires CMake 3.16+ and a C++17 compiler. GLFW, Dear ImGui, ImPlot, and doctest are all fetched
automatically via `FetchContent` — no system installs needed.

```bash
cmake -B build -DBUILD_GUI=ON
cmake --build build
```

**On Windows with MinGW**, pin the generator explicitly — CMake can default to the Visual Studio
generator instead, which produces `.sln`/`.vcxproj` files that `mingw32-make` can't read:

```bash
cmake -B build -G "MinGW Makefiles" -DBUILD_GUI=ON
cmake --build build
```

Omit `-DBUILD_GUI=ON` for a CLI-only build (faster, no OpenGL/GLFW dependency).

## Running

```bash
./build/lobsim          # CLI: Monte Carlo over Synthetic + a Historical replay, prints a summary
./build/lobsim_gui       # GUI: interactive Run/Pause/Step, live charts, strategy/source switching
./build/lobsim_tests     # 15 doctest cases
```

**Historical data**: drop trade-tape CSVs (columns: `id, price, qty, quoteQty, time, isBuyerMaker`
— the standard Binance trades export format) into a `data/` directory at the repo root. The GUI
scans `data/*.csv` at startup and lists them in a picker once you select **Historical**; the CLI
looks for `data/BTCUSDT-trades-2026-08-17.csv` by default (see `main.cpp`). This directory is
gitignored — trade data files run into the hundreds of megabytes each and don't belong in git
history.

## Design notes worth knowing before reading the code

- **Quantity scaling**: BTC trade sizes are fractional (`0.00047` BTC), but `Order::quantity` is
  `int64_t`. Historical mode scales real quantities by ×1,000,000 on the way in and divides back
  out at every display/PnL boundary — a documented tradeoff, not an accident, and the reason
  Historical-mode numbers look enormous if you print them unscaled.
- **Reference price**: `MarketMaker::referencePrice()` is the one place that decides what price
  to mark/quote against. It defaults to the live book's mid, but during Historical replay it
  switches to an EWMA of real trade prices instead (`setUseTradeBasedReference`) — because
  nothing in a replay ever cancels a resting order, so the book accumulates stale liquidity a
  real exchange would have pulled, and marking against a stale book manufactures P&L that isn't
  real. Every P&L/exposure/kill-switch calculation goes through this one method so marks and
  quotes can't disagree with each other.
- **Adverse selection**: judged one fill in arrears — a fill can only be classed as adverse once
  a *later* price observation exists to compare it against, so the denominator (`judgedFillCount`)
  is deliberately not the same as the raw fill count.

## Tests

```bash
./build/lobsim_tests
```

15 cases / 54 assertions, covering order book price-time priority, partial fills, cancel
correctness, matching-engine multi-level walks, and cost-basis accounting — including the
long-to-short position flip in a single fill, which is where cost-basis bugs like to hide.

## Known limitations

- **No queue position modelling** — a resting order is always assumed to be at the front of the
  queue and fills in full the instant a marketable order arrives. This is the single biggest
  source of optimism in the historical fill rate.
- **No latency** — a quote decided at tick *t* is live at tick *t*, not *t + k*.
- **Single-run only** — the GUI compares one strategy against one data source at a time; there's
  no multi-strategy P&L comparison chart or parameter-sweep view yet (see `ROADMAP.md` Phase
  6.3 / 7 for the planned shape of that).
- **Windows/MinGW-tested build** — CMake should work cross-platform, but the toolchain has only
  been exercised on Windows with MinGW so far.

## Roadmap

`ROADMAP.md` was written early as a phased plan and most of it (Phases 0–5, most of 7) is now
done — treat it as a historical design log plus a still-relevant list of strategy ideas (Phase
6.2: Avellaneda-Stoikov, adaptive-spread) rather than a literal to-do list.
