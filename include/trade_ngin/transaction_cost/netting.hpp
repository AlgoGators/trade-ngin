// K3, the netting adjustment (T-7b-2 8b; T-4b section 6; HD 2026-09-25 item 23).
//
// A book with several sleeves stores one execution row per sleeve, each priced as if that
// sleeve traded alone (its own cost C(q_i) in total_transaction_costs). The account sends ONE
// order per symbol and day: the signed sum Q of the sleeve quantities. This header computes, for
// one symbol-day, the part of each sleeve row's own cost that the account did not pay (or,
// negative, paid on top), so the rows keep their own cost (no attribution lost) and the
// account's netting sits in its own column:
//
//   credit_total          = sum C(q_i) - C(Q),  C(0) = 0 by definition (no order is sent,
//                           the cost model is never called: the equity $1 floor cannot apply)
//   netting_adjustment_i  = credit_total split pro rata to C(q_i), in whole 1e-8 units (the
//                           Decimal scale), largest remainder, a tie to the smaller sleeve name,
//                           so the adjustments sum to credit_total exactly
//   net_cost_i            = total_transaction_costs_i - netting_adjustment_i   (derived)
//
// and the net costs of the symbol-day sum to C(Q) to the last stored digit. The sign is T-4b's
// option (i): positive when sleeves cross, negative when one account order of the summed size
// costs more than the sleeve orders priced alone (impact grows as |q|^1.5). The weight is
// C(q_i), not |q_i|: a |q| weight can hand a sleeve a negative net cost, a C(q_i) weight cannot.
//
// One row on a symbol-day: 0. Rows at different fill prices: 0 and a warning (a symbol-day is
// netted only at one price). Nothing here reads or changes a quantity, a price or a P&L.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
namespace transaction_cost {

/// One sleeve row of a symbol-day.
struct NettingLeg {
    std::string sleeve;           ///< strategy name: orders the largest-remainder tie
    double signed_quantity{0.0};  ///< BUY positive, SELL negative
    double fill_price{0.0};
    Decimal own_cost;             ///< the row's total_transaction_costs, C(q_i)
};

enum class NettingStatus {
    SINGLE,        ///< one leg: adjustment 0
    FULL_CROSS,    ///< Q = 0: each leg's adjustment is its own cost, net 0
    NETTED,        ///< Q != 0: credit_total = sum C(q_i) - C(Q), split pro rata to C(q_i)
    MIXED_PRICES,  ///< legs at different fill prices: adjustment 0, warn
    ZERO_COST      ///< Q != 0 and every leg costs 0: nothing to split, adjustment 0
};

struct NettingOutcome {
    NettingStatus status{NettingStatus::SINGLE};
    double account_quantity{0.0};     ///< Q
    Decimal account_cost;             ///< C(Q); 0 when Q = 0
    Decimal sum_own_cost;             ///< sum C(q_i)
    Decimal credit_total;             ///< the sum of the adjustments
    std::vector<Decimal> adjustment;  ///< per leg, in input order
};

/// Prices the account's order: the signed quantity Q at the symbol-day's single fill price. Must
/// be the same cost model, state and reference price the legs were priced with.
using AccountCostFn = std::function<double(double signed_quantity, double fill_price)>;

/// Nets one symbol-day. `cost_of_q` is called at most once, and never for Q = 0.
NettingOutcome net_symbol_day(const std::vector<NettingLeg>& legs, const AccountCostFn& cost_of_q);

/// One sleeve's execution row, for the grouping below.
struct SleeveExecution {
    std::string sleeve;
    ExecutionReport* report{nullptr};
};

struct NettingReport {
    std::vector<std::string> info_lines;  ///< one NETTING line per netted symbol-day
    std::vector<std::string> warn_lines;  ///< NETTING_MIXED_PRICES lines
    int symbol_days_netted{0};
};

/// Groups the rows by symbol, nets every symbol with two or more rows, and writes every row's
/// netting_adjustment (a symbol with one row: 0). The rows are ONE day's (live) or ONE bar's
/// (backtest) executions of one portfolio. `cost_of(symbol, signed Q, fill price)` prices the
/// account's order in dollars.
NettingReport apply_netting_adjustments(
    std::vector<SleeveExecution>& rows,
    const std::function<double(const std::string& symbol, double signed_quantity,
                               double fill_price)>& cost_of);

}  // namespace transaction_cost
}  // namespace trade_ngin
