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

double FairValueModel::getFairValue() const {
    if (!hasPrice) {
        return 0.0; // no observations yet
    }
    return ewmaPrice;
}