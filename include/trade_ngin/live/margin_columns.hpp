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

/// The Day T-1 finalize of a book that stores gross_leverage (the equity book; the futures
/// rows leave the column NULL): the row's gross_notional over the finalised value, the SET
/// clause ending ", ". T-8D R22.
inline std::string finalize_gross_leverage_sql(const std::string& finalised_value_sql) {
    const std::string value = "CAST((" + finalised_value_sql + ") AS numeric(15,4))";
    return "gross_leverage = CASE WHEN " + value + " > 0 THEN gross_notional / " + value +
           " ELSE 0.0 END, ";
}


/// cash_available of an equity row. A cash account's
/// cash is the portfolio value less the unrealized P&L (the shares at the day's close) less the
/// margin posted (what the shares cost): value - unrealized - margin. The unrealized gain is not
/// cash. A margin account keeps value - margin; it is decided when margin trading is turned on.
inline double equity_cash_available(double portfolio_value, double unrealized_pnl,
                                    double margin_posted, bool cash_account) {
    return portfolio_value - (cash_account ? unrealized_pnl : 0.0) - margin_posted;
}

/// The same figure as the assignment of the Day T-1 finalise UPDATE: the finalised value (the
/// expression the UPDATE assigns to current_portfolio_value), less the finalised unrealized P&L
/// on a cash account, less the row's own stored margin_posted.
inline std::string equity_cash_available_sql(const std::string& finalised_value_sql,
                                             const std::string& finalised_unrealized_sql,
                                             bool cash_account) {
    return "cash_available = (" + finalised_value_sql + ")" +
           (cash_account ? " - (" + finalised_unrealized_sql + ")" : std::string()) +
           " - COALESCE(margin_posted, 0.0) ";
}

}  // namespace trade_ngin
