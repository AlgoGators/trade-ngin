#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>

namespace trade_ngin {
// Deterministic composition only. SQL/API caller must independently bind the
// complete producer authority to the immutable confirmed preview read-set.
// This function does not read policy, clock, database or executable files.
Result<nlohmann::json> recompute_qt_equity_accounting_output(
    const nlohmann::json& decision,const nlohmann::json& selection,
    const nlohmann::json& input,const nlohmann::json& producer_authority);
}
