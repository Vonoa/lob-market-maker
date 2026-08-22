#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "Order.h"

class HistoricalDataReplay {
private:
    std::ifstream file;

public:
    explicit HistoricalDataReplay(const std::string& filepath);
    // Writes the next valid row into `out` and returns true, or returns false
    // once the file has no more usable rows left. Skips blank/malformed/header
    // rows internally rather than requiring the caller to check separately.
    bool nextOrder(Order& out);

    // False if the file couldn't be opened at all (e.g. wrong working
    // directory) - nextOrder() would otherwise fail identically to a file
    // that opened fine and is just genuinely exhausted, with no way for a
    // caller to tell those two cases apart.
    bool isOpen() const;
};
