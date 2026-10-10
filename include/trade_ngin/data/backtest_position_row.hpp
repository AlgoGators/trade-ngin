// Which position rows a backtest stores in backtest.final_positions.

#pragma once

namespace trade_ngin {

/**
 * @brief Whether PostgresDatabase::store_backtest_positions writes a position's row: a row with
 *        a quantity always; a row closed to zero only on a book that keeps closed rows (a cash
 *        book, E2-F54) and only when it carries a realised flow (the live is_dead_row rule).
 *
 * The writer's own predicate, shared with the reader of the same rows in memory: the per-symbol
 * P&L of the statistics of migration 031 is taken on the rows the run stores, no others.
 */
inline bool backtest_position_row_is_stored(double quantity, double realized_pnl,
                                            bool keep_closed_rows) {
    const bool flat = quantity < 1e-10 && quantity > -1e-10;
    if (!flat) return true;
    const bool no_flow = realized_pnl < 1e-10 && realized_pnl > -1e-10;
    return keep_closed_rows && !no_flow;
}

}  // namespace trade_ngin
