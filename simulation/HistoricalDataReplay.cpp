#include "HistoricalDataReplay.h"
#include <cctype>

// std::ifstream's constructor opens the file immediately when given a path,
// so this can just be passed straight through in the member initializer list -
// no separate .open() call needed, and no body required.
HistoricalDataReplay::HistoricalDataReplay(const std::string& filepath) : file(filepath) {}

bool HistoricalDataReplay::nextOrder(Order& out) {
    std::string line;

    // Keep reading lines until we find a valid one or run out of file entirely.
    // std::getline itself returns false once there's nothing left to read, which
    // is what ends this loop for real (not any of the `continue`s below).
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue; // trailing blank line at end of file - skip it, not a crash
        }

        std::stringstream ss(line);
        std::vector<std::string> fields;
        std::string field;
        while (std::getline(ss, field, ',')) {
            fields.push_back(field);
        }

        if (fields.size() < 6) {
            continue; // malformed row - doesn't have enough columns to be real data
        }

        // Real data rows start with a numeric trade id. A header row (or any
        // other garbage) won't, so this catches it before std::stod ever runs.
        if (!std::isdigit(static_cast<unsigned char>(fields[1][0]))) {
            continue;
        }

        try {
            double price = std::stod(fields[1]);   // "string to double"
            double rawQty = std::stod(fields[2]);  // still real BTC units here, e.g. 0.00047

            // Scale fractional BTC up into whole-number units so it fits our int64_t
            // quantity field - option 1 from our design discussion (millionths of a BTC).
            int64_t quantity = static_cast<int64_t>(rawQty * 1000000.0);

            // isBuyerMaker == "True" means the buyer was the resting/passive side,
            // so the SELLER was the aggressor here - this behaves like a taker SELL.
            // isBuyerMaker == "False" means the buyer was the aggressor - a taker BUY.
            OrderSide side = (fields[5] == "True") ? OrderSide::SELL : OrderSide::BUY;

            out = createOrder(side, price, quantity);
            return true;
        } catch (const std::exception&) {
            // stod couldn't parse this row as a number - skip it and try the next line
            // rather than letting the exception crash the whole program.
            continue;
        }
    }

    return false; // genuinely out of usable rows
}

bool HistoricalDataReplay::isOpen() const {
    return file.is_open();
}
