#pragma once
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/portfolio/component_book.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include <nlohmann/json.hpp>
#include <vector>

namespace trade_ngin {
struct QtEquityCostTrace {
    ComponentPositionKey key;
    transaction_cost::TransactionCostResult outcome;
    transaction_cost::CostChargeObservation observation;
};
// Pure owner-preserving equity arithmetic. Source identity, current admission
// and publication belong to the caller's transaction. No I/O or authority here.
// Exact quantities compose the accepted transition kernel; actual typed equity
// costs are charged once. Daily unrealized FLOW differs from current unrealized.
Result<nlohmann::json> produce_qt_equity_accounting(
    const nlohmann::json& decision, const nlohmann::json& selection_rows,
    const nlohmann::json& accounting_inputs,
    std::vector<QtEquityCostTrace>* cost_trace = nullptr);
}
