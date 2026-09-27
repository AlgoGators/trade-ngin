#pragma once
#include <nlohmann/json.hpp>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/consumption.hpp"
namespace trade_ngin {
// Invocation evidence only; this never asserts publication, full-run or financial readiness.
Result<nlohmann::json> project_equity_strategy_consumption(const StrategyConsumptionTrace&);
}
