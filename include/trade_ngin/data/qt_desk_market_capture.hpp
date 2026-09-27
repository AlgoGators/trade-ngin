#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
namespace trade_ngin {
// Explicit inherited connection and identifiers/lease only. All financial
// operands are read from the actual scoped market table and legacy cost state.
Result<nlohmann::json> capture_qt_desk_market_source(
    pqxx::connection& connection, const nlohmann::json& request);
// Pure recorded source proof; no clock, policy or market-data read.
Result<void> validate_qt_desk_market_capture(const nlohmann::json& source_row);
}
