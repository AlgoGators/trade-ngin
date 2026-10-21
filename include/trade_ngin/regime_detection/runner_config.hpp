#pragma once

// Database connection lookup shared by the regime runners.
//
// The runners take the connection from config/defaults.json through the
// ConfigLoader, the same way the strategy and backtest runners do. There is
// no argv or environment path: a connection string never appears on a
// command line or in a process environment.

#include "trade_ngin/core/error.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace trade_ngin {
namespace regime {

// Directories searched in order: ./config (run from the repo root) and
// ../config (run from build/).
std::vector<std::filesystem::path> default_runner_config_dirs();

// Load the "base" portfolio config from the first directory in `search_dirs`
// that holds a loadable config and return its database connection string.
// Fails if no directory loads, or if the database block is empty or still
// carries the config_template placeholders.
Result<std::string> load_runner_db_connection(
    const std::vector<std::filesystem::path>& search_dirs = default_runner_config_dirs());

}  // namespace regime
}  // namespace trade_ngin
