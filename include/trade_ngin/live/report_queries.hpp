// include/trade_ngin/live/report_queries.hpp
#pragma once

#include <string>

namespace trade_ngin {

/**
 * @brief The live runners' own read of yesterday's stored position rows for the daily report
 *        (the settled positions table of the email).
 *
 * T-8D-2 R55: a query that feeds the report reads ONE book, by its strategy_id and its
 * portfolio_id, and, on a table that carries a portfolio_type (trading.positions, migration
 * 001), the system book only: a row another writer stores under the same two ids with another
 * portfolio_type is not the engine's and is not reported. The charts' queries follow the same
 * rule (ChartQueries, src/core/chart_generator.cpp).
 *
 * `date` is the UTC date of the rows (YYYY-MM-DD). `with_sleeve_name` appends strategy_name as
 * the seventh column (the futures runners key the rows by sleeve and symbol; the equity runner
 * stores one sleeve and reads six columns).
 */
inline std::string yesterday_positions_report_query(const std::string& strategy_id,
                                                    const std::string& portfolio_id,
                                                    const std::string& date,
                                                    bool with_sleeve_name) {
    return std::string(
               "SELECT symbol, quantity, average_price, daily_realized_pnl, "
               "daily_unrealized_pnl, last_update") +
           (with_sleeve_name ? ", strategy_name " : " ") +
           "FROM trading.positions "
           "WHERE strategy_id = '" +
           strategy_id + "' AND portfolio_id = '" + portfolio_id +
           "' AND portfolio_type = 'system' AND DATE(last_update) = '" + date + "'";
}

}  // namespace trade_ngin
