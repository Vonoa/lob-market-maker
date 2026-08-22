// Milestones 1-5 done, plus mine column / fair value / fills scatter / Step
// & Reset. This pass adds: book imbalance, the remaining Live Stats numbers
// (fill rate, avg spread captured, max drawdown, throughput), a My Quotes
// panel, and a Trade Tape panel. Still NOT here: Speed/Seed/Feed/Strategy
// controls, multi-strategy comparison, or sweeps - those need the engine to
// be reconfigurable at runtime, which is real Phase 6 scope, not a small
// addition. GLFW opens a window and an OpenGL context, ImGui/ImPlot don't
// draw pixels themselves (they output vertex buffers), and
// imgui_impl_glfw/imgui_impl_opengl3 (compiled into the `imgui` target in
// CMakeLists.txt) turn those buffers into what actually appears on screen.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include "SimulationEngine.h"
#include "DefaultStrategy.h"
#include "FixedSpreadStrategy.h"

// One row of the Trade Tape. "Mine" is inferred by comparing the fill price
// against what we were quoting at the time (same price-match heuristic the
// order book's "mine" column already uses) - a fill against another party
// entirely (e.g. two RandomTrader orders matching each other) can't be
// attributed to a side this way, so it's just left unmarked.
struct TapeEntry {
    int64_t tick;
    double price;
    int64_t qty;
    bool isMine;
    bool isMineBuy; // only meaningful when isMine is true
};

int main() {
    if (!glfwInit()) {
        return 1;
    }

    // Match GL Version with GLSL version
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    // Note: removed a GLFW_OPENGL_PROFILE hint that was here - core/compat profile
    // selection is only meaningful from GL 3.2 onward, and this window is
    // requesting GL 3.0, so setting it risked context creation failing outright
    // on some drivers for no benefit.

    GLFWwindow* window = glfwCreateWindow(1280, 800, "Market Making Simulation", nullptr, nullptr);

    if (window == nullptr) {
        glfwTerminate();
        return 1;
    }
    glfwMaximizeWindow(window); // adapts to whatever screen size is actually
                                // available, rather than a hardcoded pixel size
                                // that's too small on some screens and too big on others

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION(); // catches a mismatched ImGui header/binary at
                          // compile time rather than a confusing runtime crash
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();

    // ImGui's default font renders at a fixed pixel size, so on a wide/high-
    // res display (e.g. 2880px) everything reads tiny relative to the screen
    // even though nothing is technically wrong. FontGlobalScale scales text,
    // ScaleAllSizes scales padding/spacing/button sizes to match, so the UI
    // grows as one cohesive unit instead of big text in cramped buttons.
    ImGui::GetIO().FontGlobalScale = 2.2f;
    ImGui::GetStyle().ScaleAllSizes(2.2f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    // Layout, computed once from the ACTUAL (maximized) window size rather
    // than hardcoded pixels - fixed absolute positions looked fine on a
    // ~1280x800 dev window but left roughly half a wide/high-res monitor
    // completely empty. glfwPollEvents() first makes sure the maximize from
    // above has actually been processed before we read the size back.
    glfwPollEvents();
    int windowW = 1280, windowH = 800;
    glfwGetWindowSize(window, &windowW, &windowH);

    const int margin = 10;
    // Widened along with the 2.2x font/style scale above - at the old widths,
    // labels like "Strategy Controls & Risk" and "Historical" were genuinely
    // clipping, not just looking small.
    const int leftColW = 430;
    const int midColW = 560;
    const int leftColX = margin;
    const int midColX = leftColX + leftColW + margin;
    const int rightColX = midColX + midColW + margin;
    int rightColW = windowW - rightColX - margin;
    if (rightColW < 400) rightColW = 400; // floor for a small/non-maximized window

    const int colTopY = margin;
    const int colH = windowH - margin * 2;

    // Left column: Controls, My Quotes, Live Stats stacked to fill the height.
    // Scaled to match ScaleAllSizes(2.2) above - every row of text/buttons is
    // ~2.2x taller than an unscaled UI, so heights need to scale with it too.
    const int controlsH = 760; // Run/Step/Reset, Speed, source toggle, strategy toggle, Seed, State/Tick/Mid
    const int myQuotesH = 320;
    const int liveStatsY = colTopY + controlsH + margin + myQuotesH + margin;
    const int liveStatsH = colH - controlsH - myQuotesH - margin * 2;

    // Middle column: Order Book gets a bit more than half, Trade Tape the rest.
    const int orderBookH = static_cast<int>(colH * 0.55);
    const int tapeY = colTopY + orderBookH + margin;
    const int tapeH = colH - orderBookH - margin;

    // Right column: Market chart gets more height than Inventory - it's the
    // "money plot" (mid/fair value/quote band/fills), Inventory is simpler.
    const int marketH = static_cast<int>(colH * 0.65);
    const int inventoryY = colTopY + marketH + margin;
    const int inventoryH = colH - marketH - margin;

    bool running = false;

    // Rolling history buffers for the real engine's data - declared outside
    // the loop, same persistence rule as everything else in this file. One
    // entry per successful step(), trimmed to maxPoints so the charts show a
    // scrolling window instead of growing forever.
    const size_t maxPoints = 5000; // ~a full screen-width of ticks before the rolling window starts trimming
    std::vector<float> tickHistory;
    std::vector<float> midHistory;
    std::vector<float> fairValueHistory;
    std::vector<float> myBidHistory;
    std::vector<float> myAskHistory;
    // Split by side (not one combined series) so the Market chart can draw
    // buys/sells as distinct green-up/red-down markers - GUI_PANELS.md calls
    // this out specifically ("are fills spread evenly on both sides?" is one
    // of the four things that panel is meant to answer at a glance, which a
    // single undifferentiated marker can't show). Only OUR fills go in here
    // (same isMine price-match heuristic as the Trade Tape below) - a trade
    // between two other parties isn't "your fill" in the sense this chart means.
    std::vector<float> buyFillHistory;  // NaN except on ticks where OUR bid got hit
    std::vector<float> sellFillHistory; // NaN except on ticks where OUR ask got hit
    std::vector<float> inventoryHistory;

    // Trade tape - separate from the dense per-tick history above, since most
    // ticks have no fill at all.
    std::vector<TapeEntry> tape;
    const size_t maxTapeEntries = 50;

    // Max-drawdown tracking - not in SimSnapshot (that only carries this
    // tick's state), so tracked here the same way Recorder does internally:
    // running peak of totalPnL, drawdown = peak - current, keep the largest.
    double peakPnL = 0.0;
    bool hasPeak = false;
    double maxDrawdownValue = 0.0;

    // Throughput: ticks actually advanced per wall-clock second, measured
    // over a rolling ~0.5s window rather than per-frame (which would just
    // read a noisy ~60, since at most one step() happens per rendered frame).
    double throughputWindowStart = glfwGetTime();
    int64_t tickAtWindowStart = 0;
    double currentThroughput = 0.0;

    // Every *.csv in LOBSIM_DATA_DIR (an absolute path baked in at compile
    // time - see CMakeLists.txt) is a selectable Historical data file. Scanned
    // once at startup, not per-frame - the directory isn't expected to change
    // while the GUI is running, and ImGui::Combo needs stable const char*
    // pointers to live as long as the widget does, which dataFileLabels below
    // provides by owning the strings for the whole program lifetime.
    std::vector<std::string> dataFilePaths;
    std::vector<std::string> dataFileLabels;
    for (const auto& entry : std::filesystem::directory_iterator(LOBSIM_DATA_DIR)) {
        if (entry.path().extension() == ".csv") {
            dataFilePaths.push_back(entry.path().string());
            dataFileLabels.push_back(entry.path().filename().string());
        }
    }
    std::sort(dataFilePaths.begin(), dataFilePaths.end());
    std::sort(dataFileLabels.begin(), dataFileLabels.end());
    std::vector<const char*> dataFileLabelPtrs;
    for (const std::string& label : dataFileLabels) {
        dataFileLabelPtrs.push_back(label.c_str());
    }
    int selectedDataFile = 0;

    // Config is shared by both the initial engine and every Reset - kept
    // outside the engine itself so Reset can rebuild from the same settings.
    SimConfig config;
    config.source = SimConfig::Source::Synthetic;
    config.maxTicks = 10000000; // a few million - "basically unlimited" for interactive use
    config.dataPath = dataFilePaths.empty() ? "" : dataFilePaths[0];

    // Both strategy objects exist simultaneously so switching between them is
    // just a matter of pointing currentStrategy at a different one - neither
    // needs constructing/destroying to switch, only the engine does (since
    // SimulationEngine takes its Strategy by reference at construction and
    // can't be re-pointed afterward).
    DefaultStrategy defaultStrategy;
    FixedSpreadStrategy fixedSpreadStrategy;
    Strategy* currentStrategy = &defaultStrategy;

    // Ticks advanced per rendered frame while running - the "Speed" control.
    // Decoupling this from the frame rate (vsync-capped ~60fps) is what lets
    // a run actually get somewhere fast instead of being limited to ~60
    // ticks/sec forever, matching the mockup's "4x" style speed dropdown.
    int ticksPerFrame = 1;

    // unique_ptr, not a plain stack object - Reset needs to destroy the old
    // engine and build a fresh one from scratch (SimulationEngine has no
    // "rewind" of its own, and can't be reassigned since it holds a reference
    // to strategy). Constructing a brand new one and swapping the pointer
    // sidesteps needing any of that inside SimulationEngine itself.
    std::unique_ptr<SimulationEngine> engine = std::make_unique<SimulationEngine>(config, *currentStrategy);

    // Shared by both the per-frame running loop and the Step button, so
    // stepping manually while paused shows up on the charts exactly the same
    // way as a tick advanced by Run would.
    auto recordTick = [&]() {
        const SimSnapshot& s = engine->snapshot();

        tickHistory.push_back(static_cast<float>(s.tick));
        midHistory.push_back(static_cast<float>(s.mid));
        // Same 0.0-as-sentinel issue as myBid/myAsk below: getFairValue()
        // explicitly returns 0.0 before the first trade has ever happened
        // ("no observations yet"), not a real price.
        fairValueHistory.push_back(s.fairValue == 0.0 ? NAN : static_cast<float>(s.fairValue));
        // myBid/myAsk are 0.0 specifically when NOT currently quoting that
        // side (e.g. the tick the kill switch trips) - a real sentinel, not
        // a real price. Recording it as a literal 0 drags ImPlot's auto-fit
        // Y-range down to include 0, which squashes the rest of a ~$99-101
        // range into a sliver at the top of the chart. NaN instead reads as
        // "no data here" - ImPlot skips it in both auto-fit and drawing.
        myBidHistory.push_back(s.myBid == 0.0 ? NAN : static_cast<float>(s.myBid));
        myAskHistory.push_back(s.myAsk == 0.0 ? NAN : static_cast<float>(s.myAsk));

        // Classified once here, reused below for the Trade Tape entry too -
        // same "does this fill's price match what we were quoting" heuristic
        // in both places, just computed a single time.
        bool fillIsMineBuy = false;
        bool fillIsMineSell = false;
        if (s.hadFillThisTick) {
            if (s.myBid != 0.0 && s.lastFillPrice == s.myBid) {
                fillIsMineBuy = true;
            } else if (s.myAsk != 0.0 && s.lastFillPrice == s.myAsk) {
                fillIsMineSell = true;
            }
        }
        buyFillHistory.push_back(fillIsMineBuy ? static_cast<float>(s.lastFillPrice) : NAN);
        sellFillHistory.push_back(fillIsMineSell ? static_cast<float>(s.lastFillPrice) : NAN);
        // Same Historical-mode x1,000,000 quantity scaling as everywhere else -
        // computed here rather than passed in, since recordTick() is a lambda
        // declared before the main loop's own qtyScale exists.
        double recordScale = (config.source == SimConfig::Source::Historical) ? 1000000.0 : 1.0;
        inventoryHistory.push_back(static_cast<float>(s.inventory / recordScale));

        if (tickHistory.size() > maxPoints) {
            tickHistory.erase(tickHistory.begin());
            midHistory.erase(midHistory.begin());
            fairValueHistory.erase(fairValueHistory.begin());
            myBidHistory.erase(myBidHistory.begin());
            myAskHistory.erase(myAskHistory.begin());
            buyFillHistory.erase(buyFillHistory.begin());
            sellFillHistory.erase(sellFillHistory.begin());
            inventoryHistory.erase(inventoryHistory.begin());
        }

        // Max drawdown - same running-peak approach as Recorder::record().
        if (!hasPeak || s.totalPnL > peakPnL) {
            peakPnL = s.totalPnL;
            hasPeak = true;
        }
        double drawdown = peakPnL - s.totalPnL;
        if (drawdown > maxDrawdownValue) {
            maxDrawdownValue = drawdown;
        }

        // Trade tape - only on ticks with an actual fill.
        if (s.hadFillThisTick) {
            TapeEntry entry;
            entry.tick = s.tick;
            entry.price = s.lastFillPrice;
            entry.qty = s.lastFillQty;
            entry.isMine = fillIsMineBuy || fillIsMineSell;
            entry.isMineBuy = fillIsMineBuy;
            tape.push_back(entry);
            if (tape.size() > maxTapeEntries) {
                tape.erase(tape.begin());
            }
        }
    };

    // Shared by the Reset button AND the source toggle below - switching
    // Synthetic/Historical needs a full rebuild anyway (different data feed
    // entirely), so it reuses the exact same "clear everything, build a
    // fresh engine from the current config" logic as a plain Reset.
    auto doReset = [&]() {
        running = false;
        tickHistory.clear();
        midHistory.clear();
        fairValueHistory.clear();
        myBidHistory.clear();
        myAskHistory.clear();
        buyFillHistory.clear();
        sellFillHistory.clear();
        inventoryHistory.clear();
        tape.clear();
        hasPeak = false;
        peakPnL = 0.0;
        maxDrawdownValue = 0.0;
        throughputWindowStart = glfwGetTime();
        tickAtWindowStart = 0;
        currentThroughput = 0.0;
        // Old engine destroyed, brand new one built from the current config
        // and whichever strategy is currently selected - see the comment on
        // the unique_ptr declaration for why this is how Reset works rather
        // than something built into the engine.
        engine = std::make_unique<SimulationEngine>(config, *currentStrategy);
    };

    while (!glfwWindowShouldClose(window)) {

        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // ticksPerFrame (the Speed control) decouples sim speed from the
        // render rate - vsync caps this loop at ~60 frames/sec regardless,
        // so without this a "running" sim could never advance faster than
        // ~60 ticks/sec no matter how fast the underlying step() call is.
        // The `running` check inside the loop (not just around it) matters:
        // if step() fails partway through a multi-tick frame, later
        // iterations must not still try to advance.
        if (running) {
            for (int i = 0; i < ticksPerFrame && running; ++i) {
                bool stepSucceeded = engine->step();
                if (!stepSucceeded) {
                    // maxTicks reached, or the kill switch halted the run (see
                    // SimulationEngine::step()) - either way, stop advancing and
                    // let the button visibly reflect that the run genuinely ended.
                    running = false;
                } else {
                    recordTick();
                }
            }
        }

        // Throughput window check - runs every frame regardless of `running`,
        // so a paused sim correctly settles at 0 rather than showing a stale
        // number from before the last pause.
        {
            double now = glfwGetTime();
            if (now - throughputWindowStart >= 0.5) {
                int64_t ticksNow = engine->snapshot().tick;
                currentThroughput = (ticksNow - tickAtWindowStart) / (now - throughputWindowStart);
                throughputWindowStart = now;
                tickAtWindowStart = ticksNow;
            }
        }

        // Read once per frame - every panel below looks at the same
        // snapshot, so they all show a consistent view of one instant.
        const SimSnapshot& snapshot = engine->snapshot();

        // Historical mode scales real (fractional) BTC quantities x1,000,000
        // to fit into int64 units (the original quantity-scaling decision) -
        // every dollar amount and raw quantity coming out of the engine in
        // that mode is inflated by the same factor. main.cpp's CLI always
        // divides back out before printing; this scale factor does the same
        // for every dollar/quantity value displayed below. Prices themselves
        // are NOT scaled (only quantity is), so mid/bid/ask/fair value need
        // no adjustment - only qty * price derived numbers do.
        double qtyScale = (config.source == SimConfig::Source::Historical) ? 1000000.0 : 1.0;

        // Default grid layout, applied only the FIRST time each window is
        // ever seen (ImGuiCond_FirstUseEver) - imgui.ini persists whatever
        // the user drags things to afterward, across runs.

        // --- Controls panel --------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(leftColX), static_cast<float>(colTopY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(leftColW), static_cast<float>(controlsH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("Strategy Controls & Risk");

        // Single toggle button instead of two separate Run/Pause buttons -
        // the label itself reflects current state, and one click flips it.
        if (ImGui::Button(running ? "Pause Sim" : "Run Sim", ImVec2(120, 0))) {
            running = !running;
        }
        ImGui::SameLine();
        if (ImGui::Button("Step", ImVec2(60, 0))) {
            running = false; // a manual step implies "not auto-running"
            if (engine->step()) {
                recordTick();
            }
        }

        if (ImGui::Button("Reset", ImVec2(120, 0))) {
            doReset();
        }

        // Speed: ticks advanced per rendered frame, not a playback-speed
        // multiplier on wall-clock time - "4x" here means 4 ticks/frame, not
        // literally 4x real-time (that would depend on the actual tick rate
        // of whatever feed is running, which varies a lot between Synthetic
        // and a real trade-by-trade CSV replay).
        ImGui::SetNextItemWidth(220);
        ImGui::SliderInt("Speed", &ticksPerFrame, 1, 500, "%d ticks/frame");

        // Source toggle - switching feeds needs a full rebuild anyway (a
        // completely different order source), so each option also applies
        // that source's own risk-limit preset before resetting. These match
        // main.cpp's runSimulation()/runHistoricalSimulation() presets
        // exactly - Historical runs at real BTC scale (quoteSize 2000,
        // maxInventory 50000, ...), which is why this can't be just flipping
        // the enum on its own.
        ImGui::Separator();
        if (ImGui::RadioButton("Synthetic", config.source == SimConfig::Source::Synthetic)) {
            config.source = SimConfig::Source::Synthetic;
            config.quoteSize = 10;
            config.maxInventory = 100;
            config.maxOrderSize = 50;
            config.maxLoss = 1000.0;
            config.maxExposure = 5000.0;
            doReset();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Historical", config.source == SimConfig::Source::Historical)) {
            config.source = SimConfig::Source::Historical;
            if (!dataFilePaths.empty()) {
                config.dataPath = dataFilePaths[selectedDataFile];
            }
            config.quoteSize = 2000;
            config.maxInventory = 50000;
            config.maxOrderSize = 5000;
            config.maxLoss = 1000000000.0;
            config.maxExposure = 5000000000.0;
            doReset();
        }

        // Data file picker - only meaningful once Historical is selected.
        // Changing the selection rebuilds the engine against the new file the
        // same way the Source/Strategy toggles do.
        if (config.source == SimConfig::Source::Historical) {
            if (dataFilePaths.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No .csv files found in data/");
            } else {
                ImGui::SetNextItemWidth(leftColW - 20.0f);
                if (ImGui::Combo("Data file", &selectedDataFile, dataFileLabelPtrs.data(),
                                  static_cast<int>(dataFileLabelPtrs.size()))) {
                    config.dataPath = dataFilePaths[selectedDataFile];
                    doReset();
                }
            }
        }

        // Strategy selector - same "swap what currentStrategy points at, then
        // rebuild the engine" pattern as the source toggle. Strategy objects
        // themselves are cheap and stateless enough to just leave both
        // existing permanently rather than construct/destroy on switch.
        ImGui::Separator();
        if (ImGui::RadioButton("DefaultStrategy", currentStrategy == static_cast<Strategy*>(&defaultStrategy))) {
            currentStrategy = &defaultStrategy;
            doReset();
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("FixedSpread", currentStrategy == static_cast<Strategy*>(&fixedSpreadStrategy))) {
            currentStrategy = &fixedSpreadStrategy;
            doReset();
        }

        // Seed only means anything for Synthetic (RandomTrader's RNG seed) -
        // hidden for Historical, where the feed is just whatever's in the CSV.
        if (config.source == SimConfig::Source::Synthetic) {
            static int seedValue = static_cast<int>(config.seed);
            ImGui::SetNextItemWidth(220);
            ImGui::InputInt("Seed", &seedValue);
            // IsItemDeactivatedAfterEdit(), not the InputInt() return value -
            // that return fires on every keystroke as the user types, which
            // would doReset() mid-edit. This only fires once, when the field
            // loses focus (Enter or clicking away) after an actual change.
            if (ImGui::IsItemDeactivatedAfterEdit() && seedValue >= 0) {
                config.seed = static_cast<unsigned>(seedValue);
                doReset();
            }
        }

        ImGui::Text("State: %s", running ? "running" : "paused");
        ImGui::Text("Tick: %lld", static_cast<long long>(snapshot.tick));
        ImGui::Text("Mid: $%.2f", snapshot.mid);

        if (!engine->isDataSourceReady()) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed to open data file:");
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", config.dataPath.c_str());
            ImGui::TextWrapped("Launch the exe from the folder containing this CSV (the repo root), or move the CSV next to the exe.");
        }

        ImGui::End();

        // --- My Quotes panel ---------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(leftColX), static_cast<float>(colTopY + controlsH + margin)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(leftColW), static_cast<float>(myQuotesH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("My Quotes");

        ImGui::Text("Bid: %s", snapshot.myBid != 0.0 ? "" : "(not quoting)");
        if (snapshot.myBid != 0.0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "$%.2f", snapshot.myBid);
        }
        ImGui::Text("Ask: %s", snapshot.myAsk != 0.0 ? "" : "(not quoting)");
        if (snapshot.myAsk != 0.0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "$%.2f", snapshot.myAsk);
        }

        bool bothSidesQuoting = (snapshot.myBid != 0.0 && snapshot.myAsk != 0.0);
        double quotedSpread = bothSidesQuoting ? (snapshot.myAsk - snapshot.myBid) : 0.0;
        ImGui::Text("Quoted spread: $%.2f", quotedSpread);

        // How far the center of our own quote sits from true mid - a rough
        // read on how hard the strategy is currently skewing, not an exact
        // reservation-price readout (that's internal to the strategy).
        double quoteCenter = bothSidesQuoting ? (snapshot.myBid + snapshot.myAsk) / 2.0 : snapshot.mid;
        ImGui::Text("Skew (vs mid): $%.3f", quoteCenter - snapshot.mid);

        int openOrders = (snapshot.myBid != 0.0 ? 1 : 0) + (snapshot.myAsk != 0.0 ? 1 : 0);
        ImGui::Text("Open orders: %d", openOrders);

        ImGui::End();

        // --- Live stats panel -------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(leftColX), static_cast<float>(liveStatsY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(leftColW), static_cast<float>(liveStatsH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("Live Stats");

        ImGui::Text("Inventory: %.4f / %.4f", snapshot.inventory / qtyScale,
                    snapshot.inventoryLimit / qtyScale);
        ImGui::Text("Cash: $%.2f", snapshot.cash / qtyScale);

        ImGui::Separator();
        ImGui::Text("Realised PnL:    $%.2f", snapshot.realisedPnL / qtyScale);
        ImGui::Text("Unrealised PnL:  $%.2f", snapshot.unrealisedPnL / qtyScale);
        ImGui::TextColored(snapshot.totalPnL >= 0.0 ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                            "Total PnL:       $%.2f", snapshot.totalPnL / qtyScale);
        ImGui::Text("  spread PnL:    $%.2f", snapshot.spreadPnL / qtyScale);
        ImGui::Text("  inventory PnL: $%.2f", snapshot.inventoryPnL / qtyScale);

        ImGui::Separator();
        ImGui::Text("Exposure: $%.2f", snapshot.exposure / qtyScale);
        ImGui::Text("Adverse selection: %.1f%%", snapshot.adverseSelectionRatio * 100.0);
        ImGui::Text("Fills: %lld", static_cast<long long>(snapshot.fillCount));
        ImGui::Text("Fills (b/a): %lld (%lld/%lld)", static_cast<long long>(snapshot.fillCount),
                    static_cast<long long>(snapshot.buyFillCount), static_cast<long long>(snapshot.sellFillCount));
        // fillRate/avgSpreadCaptured mirror what Recorder::summary() computes
        // for a CLI run (fillCount/ticksRun, spreadPnL/fillCount) - just read
        // directly off the live snapshot instead of a completed RunSummary,
        // since this GUI loop doesn't use Recorder at all.
        double fillRate = snapshot.tick > 0
            ? static_cast<double>(snapshot.fillCount) / static_cast<double>(snapshot.tick)
            : 0.0;
        double avgSpreadCaptured = snapshot.fillCount > 0
            ? snapshot.spreadPnL / static_cast<double>(snapshot.fillCount)
            : 0.0;
        ImGui::Text("Fill rate: %.1f%%", fillRate * 100.0);
        ImGui::Text("Avg spread captured: $%.4f", avgSpreadCaptured / qtyScale);
        ImGui::Text("Max drawdown: $%.2f", maxDrawdownValue / qtyScale);
        ImGui::Text("Throughput: %.0f ticks/s", currentThroughput);

        ImGui::Separator();
        if (snapshot.killSwitchActive) {
            ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "KILL SWITCH ACTIVE");
        } else {
            ImGui::TextDisabled("Kill switch: inactive");
        }

        ImGui::End();

        // --- Order book ladder, from real data, with a "mine" column --------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(midColX), static_cast<float>(colTopY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(midColW), static_cast<float>(orderBookH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("Order Book");

        ImGui::TextDisabled("L2 · 10 levels");

        // Book imbalance: (bidVolume - askVolume) / (bidVolume + askVolume),
        // computed directly from the depth already in the snapshot - +1 means
        // all volume is on the bid side, -1 all on the ask side, 0 balanced.
        {
            int64_t bidVol = 0;
            for (const BookLevel& level : snapshot.bidLevels) bidVol += level.totalQuantity;
            int64_t askVol = 0;
            for (const BookLevel& level : snapshot.askLevels) askVol += level.totalQuantity;
            int64_t totalVol = bidVol + askVol;
            double imbalance = totalVol > 0
                ? static_cast<double>(bidVol - askVol) / static_cast<double>(totalVol)
                : 0.0;
            ImGui::Text("Imbalance: %+.2f", imbalance);
        }

        // Largest resting level on screen, either side - the denominator for
        // each row's depth bar below. GUI_PANELS.md: "the tinted bar behind
        // each row is that level's size relative to the largest level on
        // screen - a quick visual read of where the depth is."
        int64_t maxLevelVolume = 0;
        for (const BookLevel& level : snapshot.askLevels) maxLevelVolume = std::max(maxLevelVolume, level.totalQuantity);
        for (const BookLevel& level : snapshot.bidLevels) maxLevelVolume = std::max(maxLevelVolume, level.totalQuantity);

        // Draws a filled bar behind the CURRENT cell, left-aligned, width
        // proportional to qty/maxLevelVolume - called right after
        // TableNextColumn() for the Size column, before that column's Text()
        // call, so the bar sits behind the number rather than over it.
        auto drawDepthBar = [&](int64_t qty, ImU32 color) {
            if (maxLevelVolume <= 0) return;
            float fraction = static_cast<float>(qty) / static_cast<float>(maxLevelVolume);
            ImVec2 cellMin = ImGui::GetCursorScreenPos();
            float cellWidth = ImGui::GetContentRegionAvail().x;
            float cellHeight = ImGui::GetTextLineHeightWithSpacing();
            ImGui::GetWindowDrawList()->AddRectFilled(
                cellMin, ImVec2(cellMin.x + cellWidth * fraction, cellMin.y + cellHeight), color);
        };
        const ImU32 askDepthColor = IM_COL32(255, 80, 80, 55);
        const ImU32 bidDepthColor = IM_COL32(80, 255, 80, 55);

        if (ImGui::BeginTable("OrderBookTable", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Mine");
            ImGui::TableSetupColumn("Price");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Orders");
            ImGui::TableHeadersRow();

            // askLevels is stored best-first (lowest ask first). Walking it
            // in reverse puts the worst ask at the top and the best (lowest)
            // ask closest to the spread row - the usual ladder layout.
            for (auto it = snapshot.askLevels.rbegin(); it != snapshot.askLevels.rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                // snapshot.myAsk is the exact price MarketMaker last quoted on
                // this side (or 0.0 if not currently quoting) - the same
                // value that would have produced an order resting at this
                // level, so an exact match identifies "this level includes
                // my own order" without needing per-order ownership data.
                if (snapshot.myAsk != 0.0 && it->price == snapshot.myAsk) {
                    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "* %.4f",
                                        config.quoteSize / qtyScale);
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%.2f", it->price);
                ImGui::TableNextColumn();
                drawDepthBar(it->totalQuantity, askDepthColor);
                ImGui::Text("%.4f", it->totalQuantity / qtyScale);
                ImGui::TableNextColumn();
                ImGui::Text("%d", it->orderCount);
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("-- spread: %.2f --", snapshot.spread);
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();

            // bidLevels is already best-first (highest bid first), so no
            // reversal needed - the best bid lands right under the spread row.
            for (const BookLevel& level : snapshot.bidLevels) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (snapshot.myBid != 0.0 && level.price == snapshot.myBid) {
                    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "* %.4f",
                                        config.quoteSize / qtyScale);
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "%.2f", level.price);
                ImGui::TableNextColumn();
                drawDepthBar(level.totalQuantity, bidDepthColor);
                ImGui::Text("%.4f", level.totalQuantity / qtyScale);
                ImGui::TableNextColumn();
                ImGui::Text("%d", level.orderCount);
            }

            ImGui::EndTable();
        }

        ImGui::End();

        // --- Trade tape panel ---------------------------------------------------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(midColX), static_cast<float>(tapeY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(midColW), static_cast<float>(tapeH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("Trade Tape");

        if (ImGui::BeginTable("TapeTable", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg,
                               ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
            ImGui::TableSetupColumn("Tick");
            ImGui::TableSetupColumn("Price");
            ImGui::TableSetupColumn("Qty");
            ImGui::TableSetupColumn("");
            ImGui::TableHeadersRow();

            // Newest first - iterate the tape backwards.
            for (auto it = tape.rbegin(); it != tape.rend(); ++it) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%lld", static_cast<long long>(it->tick));
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", it->price);
                ImGui::TableNextColumn();
                ImGui::Text("%.4f", it->qty / qtyScale);
                ImGui::TableNextColumn();
                if (it->isMine) {
                    ImGui::TextColored(it->isMineBuy ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                        it->isMineBuy ? "MINE BUY" : "MINE SELL");
                }
            }

            ImGui::EndTable();
        }

        ImGui::End();

        // --- Market chart: mid + fair value + quote band + fills -----------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(rightColX), static_cast<float>(colTopY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(rightColW), static_cast<float>(marketH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("Live Market Analytics");

        // Header row: title on the left, visible tick range on the right -
        // matches the mockup's "tick 41,028 -> 41,208" readout. Pulled from
        // tickHistory's own front/back rather than snapshot.tick, since that's
        // the actual range currently plotted (post-maxPoints trim), not just
        // wherever the run currently is.
        ImGui::Text("MARKET - mid, fair value & my quotes");
        if (!tickHistory.empty()) {
            char rangeLabel[64];
            std::snprintf(rangeLabel, sizeof(rangeLabel), "tick %lld -> %lld",
                          static_cast<long long>(tickHistory.front()), static_cast<long long>(tickHistory.back()));
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(rangeLabel).x - 20.0f);
            ImGui::TextDisabled("%s", rangeLabel);
        }

        if (ImPlot::BeginPlot("##Market", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Tick", "Price ($)", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

            if (!tickHistory.empty()) {
                // Shaded region between myBid and myAsk at each tick - the
                // "quote band" showing where the market maker was quoting
                // relative to the mid price line drawn on top of it.
                ImPlot::PlotShaded("Quote band", tickHistory.data(), myBidHistory.data(),
                                    myAskHistory.data(), static_cast<int>(tickHistory.size()));
                ImPlot::PlotLine("Mid", tickHistory.data(), midHistory.data(),
                                  static_cast<int>(tickHistory.size()));
                ImPlot::PlotLine("Fair value", tickHistory.data(), fairValueHistory.data(),
                                  static_cast<int>(tickHistory.size()));
                // NaN-gapped like the quote band above - only ticks with a real
                // fill on that side get a marker. Split buy/sell (up/down
                // triangle, green/red) rather than one undifferentiated series -
                // GUI_PANELS.md calls this out as one of the four things this
                // panel should answer at a glance: "are fills spread evenly on
                // both sides?" isn't readable from a single marker style.
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Up, 6, ImVec4(0.3f, 1.0f, 0.3f, 1.0f),
                                            IMPLOT_AUTO, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                ImPlot::PlotScatter("Buy fills", tickHistory.data(), buyFillHistory.data(),
                                     static_cast<int>(tickHistory.size()));
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Down, 6, ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                                            IMPLOT_AUTO, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImPlot::PlotScatter("Sell fills", tickHistory.data(), sellFillHistory.data(),
                                     static_cast<int>(tickHistory.size()));
            }

            ImPlot::EndPlot();
        }

        ImGui::End();

        // --- Inventory chart, with +-maxInventory limit lines ---------------
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(rightColX), static_cast<float>(inventoryY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(rightColW), static_cast<float>(inventoryH)), ImGuiCond_FirstUseEver);
        ImGui::Begin("Inventory");

        // Turnover: total filled volume (both sides) as a multiple of the
        // full inventory range (2*maxInventory), normalized to a per-1000-tick
        // rate ("/kt") so it's comparable across runs of different lengths.
        // Raw units cancel in the ratio, so qtyScale is only needed for the
        // pos/limit readouts themselves, not for turns.
        {
            double turnoverRatio = config.maxInventory > 0
                ? static_cast<double>(snapshot.totalFilledVolume) / (2.0 * static_cast<double>(config.maxInventory))
                : 0.0;
            double turns = snapshot.tick > 0
                ? turnoverRatio / (static_cast<double>(snapshot.tick) / 1000.0)
                : 0.0;
            ImGui::Text("pos %.4f  ·  limit +-%.4f  ·  turns %.1f/kt",
                        snapshot.inventory / qtyScale, config.maxInventory / qtyScale, turns);
        }

        if (ImPlot::BeginPlot("Inventory over time", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Tick", "Inventory", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);

            if (!tickHistory.empty()) {
                ImPlot::PlotLine("Inventory", tickHistory.data(), inventoryHistory.data(),
                                  static_cast<int>(tickHistory.size()));
            }

            // Two horizontal reference lines at +maxInventory/-maxInventory -
            // ImPlotInfLinesFlags_Horizontal draws one per Y value given,
            // spanning the full width of the plot regardless of X range.
            float limits[2] = {static_cast<float>(config.maxInventory / qtyScale),
                                static_cast<float>(-config.maxInventory / qtyScale)};
            ImPlot::PlotInfLines("Limit", limits, 2, ImPlotInfLinesFlags_Horizontal);

            ImPlot::EndPlot();
        }

        ImGui::End();

        ImGui::Render();

        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
