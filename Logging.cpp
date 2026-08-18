#include "Logging.h"

// The one real definition - actual storage for the variable lives here.
// Defaults to Silent so batch/large runs are quiet unless something opts in.
LogLevel Logging::currentLevel = LogLevel::Silent;
