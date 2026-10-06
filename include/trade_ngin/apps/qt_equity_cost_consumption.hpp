#pragma once
#include "trade_ngin/apps/qt_equity_desk_cycle.hpp"

namespace trade_ngin {
// A bounded projection of actual completed owner-scoped cost calls. No I/O,
// financial recalculation or authority admission occurs here. Null trace is
// unavailable instrumentation; an explicitly present empty trace is complete
// only when the bound financial output has no executed nonzero cost calls.
// The immutable storage boundary must bind the returned child to its input and
// financial result. Its digest excludes only the optional consumption child.
Result<nlohmann::json> project_qt_equity_cost_consumption(
    const nlohmann::json& decision, const nlohmann::json& accounting_input,
    const nlohmann::json& financial_output, const std::vector<QtEquityCostTrace>* trace);
}
