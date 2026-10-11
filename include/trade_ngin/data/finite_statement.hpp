// The numbers of a results statement, guarded: a double that is not finite is never formatted
// into the text of a statement (T-8D R12, T-8D-2 R54).
//
// std::to_string prints a NaN as `nan` and an infinity as `inf`, unquoted, which Postgres reads
// as a column name: the whole statement fails, and with it the row (a backtest whose summary
// INSERT fails stores nothing of the run). A value that is not finite is no value: the cell is
// stored without one (NULL, or the column's default where it has one) and the caller logs which
// cells. Finite values are formatted exactly as before.

#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trade_ngin {

/// A double as a statement carries it: its std::to_string text, or nothing when it is not finite.
inline std::optional<std::string> finite_sql_number(double value) {
    if (!std::isfinite(value)) return std::nullopt;
    return std::to_string(value);
}

/// The VALUES of the metric columns of a backtest.results INSERT, in the order of
/// `metric_names`, joined by ", ". A metric that is not handed in or is not finite is written
/// DEFAULT: NULL on a statistic column, the column's own default on a column that has one
/// (transaction_costs, roll_costs, total_roll_fills: 0). Its name is added to `without_value`.
inline std::string backtest_summary_metric_values(
    const std::vector<std::string>& metric_names,
    const std::unordered_map<std::string, double>& metrics,
    std::vector<std::string>* without_value = nullptr) {
    std::string values;
    for (size_t i = 0; i < metric_names.size(); ++i) {
        if (i > 0) values += ", ";
        const auto it = metrics.find(metric_names[i]);
        const auto number =
            it != metrics.end() ? finite_sql_number(it->second) : std::optional<std::string>();
        if (number) {
            values += *number;
        } else {
            values += "DEFAULT";
            if (without_value) without_value->push_back(metric_names[i]);
        }
    }
    return values;
}

/// The SET list of a trading.live_results UPDATE for its double columns, "a = 1.000000, b =
/// NULL", in the map's own order. A value that is not finite is assigned NULL and its column
/// added to `without_value`.
inline std::string live_results_update_assignments(
    const std::unordered_map<std::string, double>& updates,
    std::vector<std::string>* without_value = nullptr) {
    std::string assignments;
    for (const auto& [column, value] : updates) {
        if (!assignments.empty()) assignments += ", ";
        const auto number = finite_sql_number(value);
        assignments += column + " = " + (number ? *number : std::string("NULL"));
        if (!number && without_value) without_value->push_back(column);
    }
    return assignments;
}

/// The double columns of a trading.live_results INSERT, appended to its column and value lists
/// (each entry led by ", "), in the map's own order. A value that is not finite is left out of
/// both, so its cell stays NULL, and its column is added to `without_value`.
inline void append_live_results_double_columns(
    const std::unordered_map<std::string, double>& metrics, std::string& columns,
    std::string& values, std::vector<std::string>* without_value = nullptr) {
    for (const auto& [column, value] : metrics) {
        const auto number = finite_sql_number(value);
        if (!number) {
            if (without_value) without_value->push_back(column);
            continue;
        }
        columns += ", " + column;
        values += ", " + *number;
    }
}

/// "a, b, c" for a log line.
inline std::string joined_columns(const std::vector<std::string>& columns) {
    std::string text;
    for (const auto& column : columns) {
        if (!text.empty()) text += ", ";
        text += column;
    }
    return text;
}

}  // namespace trade_ngin
