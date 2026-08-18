#pragma once

enum class LogLevel { Silent, Summary, Trades, Debug };

namespace Logging {
    // Declared here (extern = "this exists somewhere"), defined once in Logging.cpp.
    // Every file that includes this header shares the exact same variable.
    extern LogLevel currentLevel;
}
