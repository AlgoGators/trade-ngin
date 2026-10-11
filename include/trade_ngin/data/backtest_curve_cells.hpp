// The typed cells a backtest.equity_curve row carries beside its level and risk_detail (migration
// 031: the six overlay columns), and the two pieces of the batch INSERT built from them.
//
// The curve is written as ONE multi-row INSERT, so every row names the same columns: a run that
// carries cells carries one list a point, the same columns and types in the same order on every
// point, and a cell with no value is written NULL. A run that carries none (the equity book)
// writes the statement it always wrote.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "trade_ngin/data/live_results_cell.hpp"

namespace trade_ngin {

/// One list of cells a point; empty altogether: no cell column is named.
using BacktestCurveCells = std::vector<std::vector<LiveResultsCell>>;

/**
 * @brief Why `cells` cannot be written beside `points` equity points; empty when it can.
 *
 * One list a point or none at all (any other length would write the curve without the columns
 * and lose the cells without a word); every list the first one's columns and types, in its
 * order; every type one a cell may be cast to.
 */
inline std::string backtest_curve_cells_refusal(const BacktestCurveCells& cells, size_t points) {
    if (cells.empty()) return std::string();
    if (cells.size() != points) {
        return std::to_string(cells.size()) + " cell lists for " + std::to_string(points) +
               " equity points; one per point or none";
    }
    const auto& first = cells.front();
    for (const auto& cell : first) {
        if (!live_results_cell_type_known(cell.type)) {
            return "unknown cell type " + cell.type + " on column " + cell.column;
        }
    }
    for (size_t i = 1; i < cells.size(); ++i) {
        if (cells[i].size() != first.size()) {
            return "point " + std::to_string(i) + " carries " + std::to_string(cells[i].size()) +
                   " cells, the first point " + std::to_string(first.size());
        }
        for (size_t c = 0; c < first.size(); ++c) {
            if (cells[i][c].column != first[c].column || cells[i][c].type != first[c].type) {
                return "point " + std::to_string(i) + " names " + cells[i][c].column + " " +
                       cells[i][c].type + " where the first point names " + first[c].column +
                       " " + first[c].type;
            }
        }
    }
    return std::string();
}

/// The columns the statement names after risk_detail: ", a, b, ..." (empty with no cells).
inline std::string backtest_curve_cell_columns(const BacktestCurveCells& cells) {
    std::string columns;
    if (cells.empty()) return columns;
    for (const auto& cell : cells.front()) columns += ", " + cell.column;
    return columns;
}

/// One point's values for those columns: ", <quoted value>::<type>" or ", NULL" a cell. `quote`
/// is the statement's own quoting of a text value.
inline std::string backtest_curve_cell_values(
    const std::vector<LiveResultsCell>& row,
    const std::function<std::string(const std::string&)>& quote) {
    std::string values;
    for (const auto& cell : row) {
        values += cell.value ? ", " + quote(*cell.value) + "::" + cell.type : std::string(", NULL");
    }
    return values;
}

}  // namespace trade_ngin
