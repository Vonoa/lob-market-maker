#include "Recorder.h"

#include <algorithm>
#include <cmath>

Recorder::Recorder(const std::string& path) : file(path) {
    file << "tick,timestampNs,bestBid,bestAsk,mid,myBid,myAsk,inventory,cash,realisedPnL,"
         << "unrealisedPnL,totalPnL,spreadPnL,inventoryPnL,exposure,"
         << "adverseSelectionRatio,fillCount,killSwitchActive\n";
}

void Recorder::record(const SimSnapshot& snapshot) {
    file << snapshot.tick << ',' << snapshot.timestampNs << ',' << snapshot.bestBid << ','
         << snapshot.bestAsk << ',' << snapshot.mid << ',' << snapshot.myBid << ',' << snapshot.myAsk << ','
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

        // Welford: mean/variance of PnL deltas in one stable pass.
        deltaCount++;
        double d1 = delta - deltaMean;
        deltaMean += d1 / static_cast<double>(deltaCount);
        double d2 = delta - deltaMean;
        deltaM2 += d1 * d2;

        double dtSeconds = static_cast<double>(snapshot.timestampNs - previousTimestampNs) / 1e9;
        if (dtSeconds > 0.0) {
            sumDtSeconds += dtSeconds;
        }

        peakTotalPnL = std::max(peakTotalPnL, snapshot.totalPnL);
    }
    double drawdown = peakTotalPnL - snapshot.totalPnL;
    maxDrawdown = std::max(maxDrawdown, drawdown);

    previousTotalPnL = snapshot.totalPnL;
    previousTimestampNs = snapshot.timestampNs;
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

    // Sharpe: mean / stdev of per-tick PnL deltas (Welford, see the members'
    // comment), annualized using the run's actual elapsed time when timestamps
    // are available.
    if (deltaCount > 0) {
        double variance = deltaM2 / static_cast<double>(deltaCount);
        double stdDev = variance > 0.0 ? std::sqrt(variance) : 0.0;
        if (stdDev > 0.0) {
            double perTickSharpe = deltaMean / stdDev;
            double meanSecondsPerTick = sumDtSeconds / static_cast<double>(deltaCount);
            if (meanSecondsPerTick > 0.0) {
                constexpr double secondsPerYear = 31536000.0; // stated convention, not a realism claim
                result.sharpe = perTickSharpe * std::sqrt(secondsPerYear / meanSecondsPerTick);
            } else {
                // No real elapsed time available (e.g. timestamps never wired up) -
                // fall back to the un-annualized per-tick ratio rather than fabricate a scale.
                result.sharpe = perTickSharpe;
            }
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
