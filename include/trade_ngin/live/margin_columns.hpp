// The margin and leverage cells of a trading.live_results row (T-8D section 3.14, R21).
//
// A row is written twice: by the run of its own date, on the value known then, and by the next
// run's Day T-1 finalize, which settles current_portfolio_value. The four cells that divide by
// the portfolio value (equity_to_margin_ratio, margin_cushion, net_leverage, portfolio_leverage)
// are recomputed in that finalize on the finalised value, from the row's own stored
// margin_posted, gross_notional and net_notional, so the row ends on one basis.
//
// Two of the cells have no value on a row that posts no margin (a flat book): the ratio has no
// denominator and the cushion has no requirement to be above. Such a cell is stored NULL, never
// 0 or -1: the day's write leaves the column out of its INSERT and the finalize writes NULL.

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace trade_ngin {

/// equity_to_margin_ratio: the portfolio value over the posted margin. No value when no margin
/// is posted.
inline std::optional<double> equity_to_margin_ratio_of(double portfolio_value,
                                                       double margin_posted) {
    if (!(margin_posted > 0.0)) return std::nullopt;
    return portfolio_value / margin_posted;
}

/// margin_cushion: the share of the portfolio value above the maintenance requirement,
/// (value - maintenance) / value. No value when no margin is posted.
inline std::optional<double> margin_cushion_of(double portfolio_value, double margin_posted,
                                               double maintenance_requirement) {
    if (!(margin_posted > 0.0) || !(portfolio_value > 0.0)) return std::nullopt;
    return (portfolio_value - maintenance_requirement) / portfolio_value;
}

/// The day's write: a cell with a value is named in the INSERT, a cell without one is left out
/// and stays NULL.
inline void set_margin_cells(std::unordered_map<std::string, double>& double_metrics,
                             const std::optional<double>& equity_to_margin_ratio,
                             const std::optional<double>& margin_cushion) {
    double_metrics.erase("equity_to_margin_ratio");
    double_metrics.erase("margin_cushion");
    if (equity_to_margin_ratio) double_metrics["equity_to_margin_ratio"] = *equity_to_margin_ratio;
    if (margin_cushion) double_metrics["margin_cushion"] = *margin_cushion;
}

/// The Day T-1 finalize: the SET clauses of the four cells, each ending ", ".
///   finalised_value_sql  the expression the same UPDATE assigns to current_portfolio_value. It
///                        is rounded here as the column rounds it (numeric(15,4)), so each cell
///                        is the stored cells' own quotient.
///   maintenance_sql      the row's maintenance requirement in dollars (a literal or an
///                        expression over the row's stored cells).
/// The bare column names on the right read the row's stored margin_posted, gross_notional and
/// net_notional, which the finalize does not change.
inline std::string finalize_margin_columns_sql(const std::string& finalised_value_sql,
                                               const std::string& maintenance_sql) {
    const std::string value = "CAST((" + finalised_value_sql + ") AS numeric(15,4))";
    const std::string value_dp = "CAST(" + value + " AS double precision)";
    const std::string posted = "COALESCE(margin_posted, 0.0) > 0";
    return "portfolio_leverage = CASE WHEN " + value + " > 0 THEN gross_notional / " + value +
           " ELSE 0.0 END, "
           "net_leverage = CASE WHEN " + value + " > 0 THEN net_notional / " + value +
           " ELSE 0.0 END, "
           "equity_to_margin_ratio = CASE WHEN " + posted + " THEN " + value_dp +
           " / margin_posted ELSE NULL END, "
           "margin_cushion = CASE WHEN " + posted + " AND " + value + " > 0 THEN (" + value_dp +
           " - CAST((" + maintenance_sql + ") AS double precision)) / " + value_dp +
           " ELSE NULL END, ";
}

}  // namespace trade_ngin
