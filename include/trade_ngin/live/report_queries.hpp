// include/trade_ngin/live/report_queries.hpp
#pragma once

#include <string>
#include <vector>

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


/**
 * @brief The quantity each of `symbols` held at its last stored row BEFORE `date` (T-EMAIL,
 *        HD ruling Q11): a position closed on the settled day is stored with quantity 0, and the
 *        settled table prints the quantity it had when it closed.
 *
 * One row per symbol: (symbol, quantity, average_price), the latest row dated before `date` (UTC date), for the
 * book and system portfolio the settled rows are read from.
 */
inline std::string quantity_before_report_query(const std::string& strategy_id,
                                                const std::string& portfolio_id,
                                                const std::string& date,
                                                const std::vector<std::string>& symbols) {
    std::string in_list;
    for (const auto& symbol : symbols) {
        std::string safe;
        for (const char c : symbol) {
            if (c != '\'') safe += c;
        }
        in_list += (in_list.empty() ? "'" : ", '") + safe + "'";
    }
    return "SELECT DISTINCT ON (symbol) symbol, quantity, average_price FROM trading.positions "
           "WHERE strategy_id = '" +
           strategy_id + "' AND portfolio_id = '" + portfolio_id +
           "' AND portfolio_type = 'system' AND DATE(last_update) < '" + date +
           "' AND symbol IN (" + in_list + ") ORDER BY symbol, last_update DESC";
}

}  // namespace trade_ngin
