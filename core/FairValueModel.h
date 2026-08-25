#pragma once

#include <cstdint>
#include <deque>
#include <utility>

class FairValueModel {
private:
    std::deque<double> priceHistory;
    int windowSize =20;
    double alpha = 0.8;
    double ewmaPrice = 0.0;
    bool hasPrice = false;

    // Timestamped mirror of priceHistory, populated only by the timestamped
    // recordPrice() overload below. Kept separate from priceHistory rather than
    // adding a timestamp to it directly, so getVolatility()'s existing per-event
    // semantics (and every strategy constant tuned against it) can't shift under
    // callers that haven't been updated to pass a timestamp yet.
    std::deque<std::pair<double, int64_t>> timedPriceHistory;

public:
    void recordPrice(double price);
    void recordPrice(double price, int64_t timestampNs);
    double getVolatility() const;              // per-EVENT stdev - unchanged, existing meaning
    double getVolatilityPerSecond() const;      // per-TIME stdev, Brownian-scaled by sqrt(dt)
    double getFairValue() const;
};
