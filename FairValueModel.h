#pragma once

#include <deque>

class FairValueModel {
private:
    std::deque<double> priceHistory;
    int windowSize =20;
    double alpha = 0.8;
    double ewmaPrice = 0.0;
    bool hasPrice = false;

public:
    void recordPrice(double price);
    double getVolatility() const;
    double getFairValue() const;
};
