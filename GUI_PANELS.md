# LOB-Sim GUI — what each panel shows

Panel-by-panel description of the mockup. For each: what it displays, what you're actually
watching for, and what data the engine has to hand over for it to work.

---

## Toolbar

**Shows:** Run / Pause / Step / Reset, speed multiplier, seed, data feed, active strategy, and
a tick counter (`41,208 / 250,000`).

**Purpose:** control the run. **Step** is the important one — it advances exactly one tick, which
is how you debug "why did it quote *that*?" Reset re-runs from the same seed so results are
reproducible.

**Needs:** a `bool running`, an `int ticksPerFrame` (that's what "speed" really is), and the
engine's current/total tick count.

---

## Order Book

**Shows:** the live limit order book. Asks descending from the top, bids below, with the mid
price and spread in the divider row. Each row is a price level: how much size is resting there
and how many separate orders make it up. The tinted bar behind each row is that level's size
relative to the largest level on screen — a quick visual read of where the depth is.

The `mine` column marks levels where *your* orders are resting.

**What you're watching for:** whether your quotes are at the front of the queue or buried; whether
depth is lopsided (lots of bids, thin asks = buying pressure); whether the book thins out just
before a move.

**Terms:** *mid* = (best bid + best ask) / 2, the reference "true" price. *Spread* = best ask −
best bid. *Imbalance* = how one-sided the book is, usually
`(bidVolume − askVolume) / (bidVolume + askVolume)`, so +1 is all bids, −1 all asks.

**Needs:** for each of the top N levels — price, total size, order count, and a flag for whether
you have an order there.

---

## My Quotes

**Shows:** the two orders you currently have working — bid price × size, ask price × size — plus
your quoted spread and your skew.

**Purpose:** it's your strategy's *output*, isolated. The order book shows the whole market; this
shows only your decision, so you can see what the strategy chose without hunting for it.

**Terms:** *skew* = shifting both quotes away from mid to lean against your inventory. If you're
long, you skew down (quote a lower bid and a lower ask) to make selling more likely and buying
less likely. It's the main mechanism a market maker has for controlling inventory, so watching
it move is watching the strategy think.

**Needs:** current bid/ask price and size, plus the skew value the strategy computed.

---

## Trade Tape

**Shows:** a scrolling list of executed trades — time, side, price, size — with your own fills
flagged `MINE`.

**Purpose:** raw event stream. Useful when stepping tick by tick, mostly noise when running fast.
(This is the first panel I'd cut — the fill markers on the Market chart carry the same
information in a form you can actually read at speed.)

**Needs:** a rolling list of recent trades with an "is mine" flag.

---

## Market — mid, fair value & my quotes

**The most important panel.** Four things on one price axis:

- **White line — mid price.** What the market currently says the price is.
- **Orange line — fair value.** What *your model* thinks the price really is (`FairValueModel`).
  It's usually a smoothed or imbalance-adjusted version of mid. You quote around this, not
  around mid.
- **Blue dashed lines + shaded band — your bid and ask.** The band *is* your spread, drawn to
  scale. Its width is how much edge you're asking for; its position relative to the white line
  is your skew.
- **Green / red triangles — your fills.** Up = you bought, down = you sold, placed at the price
  it happened.

**What you're watching for**, and this is why the panel earns the most space:

- Does the band **track** the mid, or lag behind it? A lagging band means you're getting picked
  off on the wrong side of every move.
- Are fills **spread evenly on both sides**? Healthy market making is buy-sell-buy-sell. All
  green in a falling market means you're catching a falling knife and your inventory is about to
  be a problem.
- Does the band **shift off-centre** when inventory builds? That's skew working correctly.
- **Adverse selection:** do fills cluster right before the price moves against you? That's the
  fundamental market-maker problem — the people who trade with you often know something.

**Needs:** rolling history arrays of mid, fair value, your bid, your ask, plus a list of fill
events (tick, price, side).

---

## Inventory

**Shows:** your net position over time, as a filled area around zero, with the ±50 position limit
drawn as dashed orange lines.

**Purpose:** inventory risk is what actually kills market makers. Every unit you hold is exposure
to a price move you didn't want. A good strategy oscillates tightly around zero; a bad one drifts
to one side and pins against the limit.

**What you're watching for:** whether it **mean-reverts** (crosses zero regularly) or **trends**
(drifts one way and stays). If it flatlines against ±50, the strategy has stopped quoting one
side entirely and is no longer market making — it's just holding a directional bet.

**Needs:** a rolling history of net position, plus the configured limit.

---

## P&L — strategy comparison

**Shows:** cumulative profit over the run, one line per strategy, all on the same axis so they
start together at zero.

**Purpose:** the actual research question — which strategy makes more money on the same data.
Same seed and same feed for every line, or the comparison means nothing.

**What you're watching for:** not just the final height. **Smoothness matters more.** A line that
climbs steadily is a real edge; one that jumps around and happens to end high got lucky. That
"smoothness" is roughly what Sharpe measures.

**Needs:** cumulative P&L history per completed run. Note this is the one panel reading *finished
runs* rather than live state — it comes from the recorder CSVs, not from `snapshot()`.

---

## Live Stats

**Shows:** the numbers for the currently selected strategy.

| Stat | Meaning |
|---|---|
| **P&L** | total profit |
| **realised / unrealised** | realised = locked in by closed round-trips. Unrealised = paper value of the inventory you're still holding, which can evaporate. The split matters: big unrealised P&L means your "profit" is really an open bet. |
| **inventory** | current net position |
| **fills (b/a)** | buy fills vs sell fills — should be roughly balanced |
| **fill rate** | share of your quotes that got hit. Too low = quoting too wide, nobody trades with you. Too high = quoting too tight, you're being run over. |
| **avg spread captured** | average edge earned per round trip. This is the market maker's actual product. |
| **max drawdown** | worst peak-to-trough fall in P&L — the "how bad did it get" number |
| **sim throughput** | ticks/sec, a performance check rather than a trading one |

**Needs:** these come straight off `MarketMaker`; most already exist.

---

## Strategy Comparison table

**Shows:** one row per completed run — P&L, Sharpe, max inventory, fill %.

**Purpose:** the summary version of the P&L chart, and the thing you'd actually screenshot for a
writeup.

**Terms:** *Sharpe* = return divided by volatility of return — profit per unit of risk taken.
Ranking by P&L alone rewards strategies that got lucky with a big position; Sharpe doesn't.
*Max inventory* is a risk measure: a strategy earning +2,000 while never exceeding 19 units is
strictly better than one earning the same by sitting at 71.

**Needs:** parsed summary rows from finished-run CSVs.

---

## Params / Sweep / Log tabs

**Shows:** the active strategy's tunable parameters as editable fields — spread, quote size,
inventory limit, skew coefficient γ, fair-value α, latency.

**Purpose:** change a parameter and see the effect immediately, without recompiling. This is the
single biggest day-to-day win of having a GUI at all.

**Sweep** runs the grid (spread 1→8 × γ 0.0→0.4 = 40 runs) as a batch and dumps results to the
comparison table — a batch job, not something you watch.

**Needs:** a params struct the GUI can write into directly. `ImGui::SliderInt("...", &params.spread, 1, 8)`
mutates it in place — no plumbing.

---

## Status bar

Current engine call, recorder output file, replay progress, frame time. Debug information;
cheap to add, occasionally saves you.

---

## What this implies for the engine interface

Collecting the "needs" above, `snapshot()` has to return roughly:

```cpp
struct BookLevel { double price; int size; int orderCount; bool mine; };

struct SimSnapshot {
    long long tick;
    std::vector<BookLevel> bids, asks;   // top N only
    double mid, spread, imbalance;
    double fairValue;
    double myBid, myAsk;  int myBidSize, myAskSize;
    double skew;
    int    inventory;
    double pnlRealised, pnlUnrealised;
    int    fillsBuy, fillsSell;
    double avgSpreadCaptured, maxDrawdown;
};
```

Everything else the GUI needs is either **history** (the GUI accumulates it frame by frame into
rolling buffers — the engine shouldn't have to store it) or **finished-run data** (read from
recorder CSVs, not from the live engine).

That split is worth keeping strict: live state comes from `snapshot()`, history is the GUI's own
problem, comparisons come from files.
