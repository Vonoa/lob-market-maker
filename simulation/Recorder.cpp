#include "Recorder.h"

#include <algorithm>
#include <cmath>

Recorder::Recorder(const std::string& path) : file(path) {
    file << "tick,bestBid,bestAsk,mid,myBid,myAsk,inventory,cash,realisedPnL,"
         << "unrealisedPnL,totalPnL,spreadPnL,inventoryPnL,exposure,"
         << "adverseSelectionRatio,fillCount,killSwitchActive\n";
}

void Recorder::record(const SimSnapshot& snapshot) {
    file << snapshot.tick << ',' << snapshot.bestBid << ',' << snapshot.bestAsk << ','
         << snapshot.mid << ',' << snapshot.myBid << ',' << snapshot.myAsk << ','
         << snapshot.inventory << ',' << snapshot.cash << ',' << snapshot.realisedPnL << ','
         << snapshot.unrealisedPnL << ',' << snapshot.totalPnL << ',' << snapshot.spreadPnL << ','
         << snapshot.inventoryPnL << ',' << snapshot.exposure << ','
         << snapshot.adverseSelectionRatio << ',' << snapshot.fillCount << ','
         << snapshot.killSwitchActive << '\n';

    // Running accumulators - one pass, computed as each tick arrives rather than
    // by re-reading the CSV afterward.
    if (tickCount == 0) {
        peakTotalPnL = snapshot.totalPnL; // first observation is the only candidate peak so far
    } else {
        double delta = snapshot.totalPnL - previousTotalPnL;
        sumPnLDelta += delta;
        sumPnLDeltaSquared += delta * delta;
        peakTotalPnL = std::max(peakTotalPnL, snapshot.totalPnL);
    }
    double drawdown = peakTotalPnL - snapshot.totalPnL;
    maxDrawdown = std::max(maxDrawdown, drawdown);

    previousTotalPnL = snapshot.totalPnL;
    sumAbsInventory += std::abs(static_cast<double>(snapshot.inventory));

    finalTotalPnL = snapshot.totalPnL;
    finalSpreadPnL = snapshot.spreadPnL;
    finalInventoryPnL = snapshot.inventoryPnL;
    finalFillCount = snapshot.fillCount;

    tickCount++;
}

RunSummary Recorder::summary() const {
    RunSummary result;
    result.totalPnL = finalTotalPnL;
    result.spreadPnL = finalSpreadPnL;
    result.inventoryPnL = finalInventoryPnL;
    result.maxDrawdown = maxDrawdown;
    result.fillCount = finalFillCount;
    result.ticksRun = tickCount;

    if (tickCount > 0) {
        result.fillRate = static_cast<double>(finalFillCount) / tickCount;
        result.timeWeightedAbsInventory = sumAbsInventory / tickCount;
    }
    if (finalFillCount > 0) {
        result.spreadCapturePerFill = finalSpreadPnL / finalFillCount;
    }

    // Sharpe: mean / stdev of per-tick PnL deltas, un-annualized (see the note
    // on RunSummary - no real tape timestamps to scale by steps-per-day yet).
    int64_t deltaCount = tickCount > 0 ? tickCount - 1 : 0; // first tick has no prior delta
    if (deltaCount > 0) {
        double mean = sumPnLDelta / deltaCount;
        double variance = sumPnLDeltaSquared / deltaCount - mean * mean;
        double stdDev = variance > 0.0 ? std::sqrt(variance) : 0.0;
        if (stdDev > 0.0) {
            result.sharpe = mean / stdDev;
        }
    }

    return result;
}

void writeSummaryJson(const RunSummary& summary, const std::string& path) {
    std::ofstream out(path);
    out << "{\n"
        << "  \"totalPnL\": " << summary.totalPnL << ",\n"
        << "  \"spreadPnL\": " << summary.spreadPnL << ",\n"
        << "  \"inventoryPnL\": " << summary.inventoryPnL << ",\n"
        << "  \"sharpe\": " << summary.sharpe << ",\n"
        << "  \"maxDrawdown\": " << summary.maxDrawdown << ",\n"
        << "  \"fillRate\": " << summary.fillRate << ",\n"
        << "  \"spreadCapturePerFill\": " << summary.spreadCapturePerFill << ",\n"
        << "  \"timeWeightedAbsInventory\": " << summary.timeWeightedAbsInventory << ",\n"
        << "  \"fillCount\": " << summary.fillCount << ",\n"
        << "  \"ticksRun\": " << summary.ticksRun << "\n"
        << "}\n";
}
