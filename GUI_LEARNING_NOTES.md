# LOB-Sim GUI — what I need to learn to build it

Notes on the skills required to build the GUI mockup, roughly in the order you'd learn them.
Stack: **Dear ImGui + ImPlot + GLFW**, pulled in via CMake FetchContent.

Effort estimates assume learning-from-scratch, not expert speed. They're deliberately
pessimistic-ish so nothing feels like a nasty surprise.

---

## 0. What you already have

Everything language-level. Structs, classes, `std::vector`, references, pointers, loops.

The GUI adds **no new C++ language features**. You will not need templates, inheritance
hierarchies, smart pointers, move semantics, or anything else you haven't already met in the sim.
The ImGui API is plain free functions.

---

## 1. CMake — REQUIRED, ~half a day

The single biggest new thing, and it's a build-system skill, not a GUI skill.

What to actually learn (this is the whole list):

- `cmake_minimum_required`, `project()`
- `add_executable(name src1.cpp src2.cpp ...)` — a *target*
- `target_link_libraries(name PRIVATE dep)` — how targets depend on each other
- `target_include_directories`
- `FetchContent_Declare` / `FetchContent_MakeAvailable` — downloads a GitHub repo at configure
  time and makes its targets available. This is how ImGui/ImPlot/GLFW arrive. No manual
  downloading, no vendored copies in the repo.
- Configure vs build: `cmake -S . -B build` then `cmake --build build`

**Why it's non-negotiable here:** ImGui has no installer and no package. FetchContent is the
standard way to get it. Separately, `.vscode/tasks.json` currently hardcodes
`C:\msys64\ucrt64\bin\g++.exe`, so nobody else can build the repo at all — CMake fixes that too.
Two benefits, one afternoon.

**Windows wrinkle to expect:** you'll need to tell CMake which generator and compiler to use with
the MSYS2/UCRT64 toolchain — likely `-G "Ninja"` or `-G "MinGW Makefiles"` from the UCRT64 shell.
This is the most likely place to lose a few hours. It's a one-time fight.

---

## 2. Immediate-mode GUI — the one genuinely new *concept*, ~1 hour to grasp

This is the mental model, and everything else follows from it.

In a normal (retained-mode) toolkit — Qt, tkinter, WinForms — you create a Button object once,
attach a callback, and then write code to keep it in sync with your data.

In immediate mode there are no objects. **Every frame, you call a function that draws the widget
and returns what the user did to it:**

```cpp
if (ImGui::Button("Run")) running = true;    // true ONLY on the frame it's clicked
ImGui::SliderInt("Spread", &params.spread, 1, 8);  // writes straight into your struct
ImGui::Text("P&L: %+.2f", mm.pnl());
```

The UI is a *function of your state*, re-run 60 times a second. Delete the `if` and the button
is gone. There is no sync code because there is nothing to sync.

Things to actually understand:
- The frame lifecycle: `NewFrame()` → your widget calls → `Render()`
- Widget return values are per-frame events, not persistent state
- **The one real gotcha: ID collisions.** ImGui identifies widgets by their label string. Two
  buttons both labelled `"Reset"` are the *same widget*. Fix with `"Reset##book"` (text after
  `##` is part of the ID but not displayed) or `PushID`/`PopID` in loops. This will bite you at
  least once — knowing it exists in advance saves an hour of confusion.

---

## 3. GLFW + OpenGL context boilerplate — copy-paste, ~1 hour

ImGui doesn't open windows and doesn't draw pixels. It outputs vertex buffers. So you need:

- **GLFW** to open an OS window and create an OpenGL context
- **The ImGui backends** (`imgui_impl_glfw.cpp`, `imgui_impl_opengl3.cpp`) — these ship *inside*
  the ImGui repo; you just add the two `.cpp` files to your target. They turn ImGui's vertex
  buffers into pixels.

**You do NOT need to learn OpenGL.** No shaders, no matrices, no graphics programming. This is
~15 lines of init and 5 lines in the loop, written once and never touched again. Understand
roughly what a window and a context are, copy the block from the ImGui example, move on.

---

## 4. The ImGui widget set — ~an afternoon

Small, and you only need a fraction of it:

| Need | Call |
|---|---|
| a panel | `ImGui::Begin("Order Book")` / `ImGui::End()` |
| buttons | `ImGui::Button` |
| params | `ImGui::SliderInt`, `SliderFloat`, `InputInt`, `Checkbox` |
| dropdowns | `ImGui::Combo` or `BeginCombo`/`EndCombo` |
| text + numbers | `ImGui::Text`, `TextColored`, `SameLine`, `Separator` |
| the order book ladder | `ImGui::BeginTable` / `TableNextRow` / `TableNextColumn` / `EndTable` |
| tabs | `ImGui::BeginTabBar` / `BeginTabItem` |

**The key skill is knowing where the docs are: `imgui_demo.cpp`.** ImGui's documentation is a
single 8000-line source file that renders an interactive demo window showing every widget. You
run the demo, find the thing that looks like what you want, and read the code that produced it.
Call `ImGui::ShowDemoWindow()` in your own app and keep it open while building.

Docking (drag panels around, split the window) is on a separate ImGui branch and is **optional** —
fixed panel positions work fine for v1.

---

## 5. ImPlot — ~2–3 hours

The charts. Same immediate-mode style. What each mockup panel needs:

- `ImPlot::BeginPlot` / `EndPlot`
- `PlotLine(label, xs, ys, count)` — mid price, fair value, inventory, P&L curves
- `PlotShaded` — the blue band between your bid and ask quotes
- `PlotScatter` — the fill markers
- `SetupAxes`, `SetupAxisLimits`, autofit vs rolling window — how to make the x-axis scroll with
  the sim rather than squashing everything

**Important:** ImPlot takes raw pointers to contiguous arrays (`double*` + count). Nothing is
pre-rendered, no images, no Python. If a number is in a `std::vector<double>`, you can plot it.

---

## 6. The C++ you'll actually write — ~a day, and the most valuable part

This is the real work and the only part that's genuinely *your* code rather than library usage.

- **A rolling history buffer.** ImPlot wants contiguous arrays, so something must hold the last
  N thousand mid prices / inventory values / P&L points. Simplest version is a `std::vector` you
  append to and trim; a proper ring buffer is nicer. Worth learning: why `push_back` in a hot
  loop can reallocate, and `reserve()`.
- **A snapshot struct.** `SimulationEngine::snapshot()` returns a plain struct with the current
  book levels, quotes, inventory, P&L. The GUI reads it; the engine knows nothing about the GUI.
  Designing this struct *is* the interface between the two halves.
- **Decoupling sim speed from frame rate.** The window redraws at 60fps but the sim wants to run
  millions of ticks/sec. So: `for (int i = 0; i < ticksPerFrame; ++i) engine.step();` and the
  speed control just changes `ticksPerFrame`. Understanding why these two rates are separate is
  the main design idea in the whole GUI.

---

## 7. Optional / later — explicitly not needed for v1

- **Threading.** Running the sim on a worker thread so the UI stays responsive during a long
  run. Genuinely harder — needs a mutex or double-buffered snapshot, and races here are painful
  to debug. Only worth it if single-threaded pacing actually feels laggy. **Skip for v1.**
- **Docking branch** — nice, not needed.
- **CSV parsing in C++** for the strategy-comparison panel, if that panel is built in the GUI
  rather than done offline in Python.
- **Packaging** — shipping an .exe that runs on a machine without MSYS2 installed.

---

## What NOT to learn (things that look relevant and aren't)

- **OpenGL / shaders / graphics programming** — the backend handles all of it.
- **Qt** — different (retained-mode) toolkit, much heavier, would mean rewriting the sim loop
  around its event system.
- **Signals/slots, callbacks, event-driven architecture** — immediate mode has none of this.
- **HTML/CSS/JS** — the mockup happens to be HTML; the real thing shares nothing with it.
- **Python, for the GUI** — no role in the render path at all. (Python is only for the separate
  offline plotting of recorder CSVs.)

---

## Suggested milestones

Each one is independently satisfying and independently debuggable:

1. **Blank window.** CMake builds, GLFW window opens, ImGui demo window shows. *This is the hard
   one* — everything after it is additive.
2. **A button that does something.** Run/Pause toggling a bool that the existing sim loop reads.
3. **One live chart** driven by a fake sine wave. Proves the ImPlot + rolling-buffer path works
   before real data is involved.
4. **The order book table** from a real `snapshot()`.
5. **Wire in the real engine** — inventory chart, stats, quote band.

Realistically: milestone 1 is an evening (most of it CMake), milestones 2–5 are a weekend if
they go smoothly.

---

## Dependency note

The GUI reads from `SimulationEngine::snapshot()`, which doesn't exist yet — it's phase 2 of the
roadmap, and the GUI is phase 7. Milestones 1–3 above need none of that (fake data is fine), so
they can be done any time as a spike. Milestones 4–5 need the engine refactor first.
