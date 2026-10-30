// include/trade_ngin/data/roll_split_sql.hpp
//
// Migration 040 in the executions writers (rolls step 4, ruling 18): a statement names the three roll split
// columns only when a row it writes carries a split, so with per_contract_rolls off every statement is the one
// before 040 and the binary does not need the migration. Header-only and pure: the writers
// (PostgresDatabase::insert_executions_in, store_backtest_executions_with_strategy) and the tests call these.
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <pqxx/pqxx>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin::roll_split_sql {

/// The row carries a split (a per-contract roll's closing leg written with the switch on).
inline bool has_split(const ExecutionReport& e) { return e.roll_bar_move.has_value(); }

/// Any row of the call carries one: the backtest's statements then name the columns on every row.
inline bool any_split(const std::vector<ExecutionReport>& rows) {
    return std::any_of(rows.begin(), rows.end(), [](const ExecutionReport& e) { return has_split(e); });
}

/// ", roll_bar_move, roll_gap_points, roll_gap_value", or nothing.
inline std::string columns(bool split) {
    return split ? ", roll_bar_move, roll_gap_points, roll_gap_value" : "";
}

/// ", $n, $n+1, $n+2" from `first` = n, or nothing.
inline std::string placeholders(bool split, int first) {
    if (!split) return "";
    return ", $" + std::to_string(first) + ", $" + std::to_string(first + 1) + ", $" + std::to_string(first + 2);
}

/// The three values in the columns' order (NULL where the row has none).
inline void append(pqxx::params& params, const ExecutionReport& e) {
    params.append(e.roll_bar_move);
    params.append(e.roll_gap_points);
    params.append(e.roll_gap_value);
}

}  // namespace trade_ngin::roll_split_sql
