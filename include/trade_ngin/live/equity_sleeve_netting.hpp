#pragma once

#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
class ExecutionManager;
}

namespace trade_ngin::live {

// One independently owned sleeve book before account-level netting.
struct EquitySleevePositionBook {
    std::string strategy_name;
    std::map<std::string, double> previous_quantities;
    std::map<std::string, double> target_quantities;
    std::map<std::string, double> reference_prices;
};

// The logical owner fill remains gross and is the only fill persisted against
// the sleeve. Costs are populated after the external account fill is known.
struct EquitySleeveLogicalFill {
    std::string strategy_name;
    std::string symbol;
    double signed_quantity{0.0};
    double reference_price{0.0};
    double as_if_transaction_costs{0.0};
    double netting_adjustment{0.0};
    double net_transaction_costs{0.0};
};

// The only order that may cross the external account boundary for a symbol.
struct EquityAccountOrder {
    std::string symbol;
    double signed_quantity{0.0};
    double reference_price{0.0};
};

struct EquitySleeveNettingPlan {
    std::vector<EquitySleeveLogicalFill> logical_fills;
    std::vector<EquityAccountOrder> account_orders;
};

struct EquityAccountExecutionCost {
    std::string symbol;
    double signed_quantity{0.0};
    double total_transaction_costs{0.0};
};

struct EquitySleeveAsIfCost {
    std::string strategy_name;
    std::string symbol;
    double signed_quantity{0.0};
    double as_if_transaction_costs{0.0};
};

struct EquitySleeveExecution {
    std::string strategy_name;
    ExecutionReport execution;
};

struct EquitySleeveExecutionPlan {
    std::vector<EquitySleeveExecution> sleeve_executions;
    std::vector<ExecutionReport> account_executions;
};

Result<EquitySleeveNettingPlan> build_equity_sleeve_netting_plan(
    const std::vector<EquitySleevePositionBook>& sleeves);

// Reconciles one actual/modelled account cost per planned external order. Each
// sleeve retains C(q_i). The credit sum(C(q_i)) - C(Q), which may be negative,
// is split pro rata by C(q_i). A full internal cross has C(Q)=0. Returned net
// costs sum exactly to the account cost for each symbol.
Result<std::vector<EquitySleeveLogicalFill>> reconcile_equity_sleeve_costs(
    const EquitySleeveNettingPlan& plan,
    const std::vector<EquitySleeveAsIfCost>& sleeve_costs,
    const std::vector<EquityAccountExecutionCost>& account_executions);

// Price both the gross owner fills and the one net external account order per
// symbol with the same cost model, then attach the ruled netting adjustment to
// each owner execution. The returned sleeve reports are the persistence and
// owner-callback records; account executions are evidence of what crossed the
// external boundary and are never attributed to a sleeve.
Result<EquitySleeveExecutionPlan> generate_equity_sleeve_executions(
    ExecutionManager& manager, const EquitySleeveNettingPlan& plan,
    const Timestamp& timestamp);

}  // namespace trade_ngin::live
