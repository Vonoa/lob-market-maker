#include "FairValueModel.h"
#include <cmath>

void FairValueModel::recordPrice(double price) {
    priceHistory.push_back(price);
    if (priceHistory.size() > static_cast<size_t>(windowSize)) {
        priceHistory.pop_front();
    }

    if (!hasPrice) {
        // First observation ever: nothing to blend against yet.
        ewmaPrice = price;
        hasPrice = true;
    } else {
        ewmaPrice = alpha * price + (1 - alpha) * ewmaPrice;
    }
}

double FairValueModel::getVolatility() const {
    if (priceHistory.size() < 2) {
        return 0.0;
    }

    // First pass: mean of the consecutive price differences.
    double sumOfDifferences = 0.0;
    for (size_t i = 1; i < priceHistory.size(); ++i) {
        sumOfDifferences += priceHistory[i] - priceHistory[i - 1];
    }
    double numDifferences = static_cast<double>(priceHistory.size() - 1);
    double mean = sumOfDifferences / numDifferences;

    // Second pass: mean squared deviation of each difference from that mean (variance).
    double sumSquaredDeviations = 0.0;
    for (size_t i = 1; i < priceHistory.size(); ++i) {
        double diff = priceHistory[i] - priceHistory[i - 1];
        double deviation = diff - mean;
        sumSquaredDeviations += deviation * deviation;
    }
    double variance = sumSquaredDeviations / numDifferences;

    return std::sqrt(variance);
}

void FairValueModel::recordPrice(double price, int64_t timestampNs) {
    recordPrice(price); // reuse the existing EWMA/priceHistory bookkeeping exactly as-is

    timedPriceHistory.push_back({price, timestampNs});
    if (timedPriceHistory.size() > static_cast<size_t>(windowSize)) {
        timedPriceHistory.pop_front();
    }
}

double FairValueModel::getVolatilityPerSecond() const {
    if (timedPriceHistory.size() < 2) {
        return 0.0;
    }

    size_t n = timedPriceHistory.size() - 1;
    double sumOfDifferences = 0.0;
    double sumOfDtSeconds = 0.0;
    for (size_t i = 1; i < timedPriceHistory.size(); ++i) {
        sumOfDifferences += timedPriceHistory[i].first - timedPriceHistory[i - 1].first;
        sumOfDtSeconds += static_cast<double>(timedPriceHistory[i].second - timedPriceHistory[i - 1].second) / 1e9;
    }
    double mean = sumOfDifferences / static_cast<double>(n);

    double sumSquaredDeviations = 0.0;
    for (size_t i = 1; i < timedPriceHistory.size(); ++i) {
        double diff = timedPriceHistory[i].first - timedPriceHistory[i - 1].first;
        double deviation = diff - mean;
        sumSquaredDeviations += deviation * deviation;
    }
    double variance = sumSquaredDeviations / static_cast<double>(n);
    double stdDev = std::sqrt(variance);

    double meanDtSeconds = sumOfDtSeconds / static_cast<double>(n);
    if (meanDtSeconds <= 0.0) {
        return 0.0; // no real elapsed time between observations - can't scale to per-second
    }

    // Brownian scaling: stdev over an interval of length dt scales with sqrt(dt),
    // so dividing by sqrt(dt) converts an observed per-interval stdev to per-second.
    return stdDev / std::sqrt(meanDtSeconds);
}

double FairValueModel::getFairValue() const {
    if (!hasPrice) {
        return 0.0; // no observations yet
    }
    return ewmaPrice;
}