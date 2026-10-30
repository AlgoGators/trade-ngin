// Which position rows a backtest stores in backtest.final_positions.

#pragma once

#include <string>

namespace trade_ngin {

/// The one value backtest.final_positions.row_mark takes (migration 038; its CHECK admits no
/// other): the row of a futures symbol that printed no bar on the row's date. The position is
/// held and marked at its last close, so the row books nothing: realized_pnl 0, quantity and
/// average_price kept.
inline constexpr const char* kRowMarkNoBar = "NO_BAR";

/// The row_mark cell of a stored row as the INSERT writes it: the mark on a no-bar row, NULL on
/// every other row.
inline std::string backtest_position_row_mark_value(bool no_bar) {
    return no_bar ? "'" + std::string(kRowMarkNoBar) + "'" : std::string("NULL");
}

/**
 * @brief Whether PostgresDatabase::store_backtest_positions writes a position's row: a row with
 *        a quantity always; a row closed to zero only on a book that keeps closed rows (a cash
 *        book) and only when it carries a realised flow (the live is_dead_row rule).
 *
 * The writer's own predicate, shared with the reader of the same rows in memory: the per-symbol
 * P&L of the statistics of migration 037 is taken on the rows the run stores, no others.
 */
inline bool backtest_position_row_is_stored(double quantity, double realized_pnl,
                                            bool keep_closed_rows) {
    const bool flat = quantity < 1e-10 && quantity > -1e-10;
    if (!flat) return true;
    const bool no_flow = realized_pnl < 1e-10 && realized_pnl > -1e-10;
    return keep_closed_rows && !no_flow;
}

}  // namespace trade_ngin
