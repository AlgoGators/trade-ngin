// src/data/postgres_database_extensions.cpp
// Phase 0: Database Extensions to Replace Raw SQL
// This file contains new methods to eliminate raw SQL from backtest and live trading

#include "trade_ngin/data/postgres_database.hpp"
#include <sstream>
#include <iomanip>

namespace trade_ngin {

// ============================================================================
// NEW METHODS TO REPLACE RAW SQL (Phase 0 Refactoring)
// ============================================================================

Result<void> PostgresDatabase::delete_stale_executions(
    const std::vector<std::string>& order_ids,
    const Timestamp& date,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    if (order_ids.empty()) {
        return Result<void>();  // Nothing to delete
    }

    try {
        pqxx::work txn(*connection_);

        // Build a safe IN (...) clause for the provided order_ids
        std::string in_list;
        in_list.reserve(order_ids.size() * 20);
        for (size_t i = 0; i < order_ids.size(); ++i) {
            if (i > 0) in_list += ", ";
            in_list += txn.quote(order_ids[i]);
        }

        std::string query = "DELETE FROM " + table_name +
                           " WHERE DATE(execution_time) = $1 AND order_id IN (" + in_list + ")";

        // Execute delete for the specified date (YYYY-MM-DD)
        txn.exec_params(query, format_timestamp(date).substr(0, 10));

        txn.commit();

        INFO("Deleted stale executions for " + std::to_string(order_ids.size()) +
             " order IDs on " + format_timestamp(date));

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete stale executions: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_backtest_summary(
    const std::string& run_id,
    const Timestamp& start_date,
    const Timestamp& end_date,
    const std::unordered_map<std::string, double>& metrics,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    try {
        pqxx::work txn(*connection_);

        // Build INSERT query with all metrics
        std::string query = "INSERT INTO " + table_name + " ("
            "run_id, start_date, end_date, total_return, sharpe_ratio, sortino_ratio, "
            "max_drawdown, calmar_ratio, volatility, total_trades, win_rate, profit_factor, "
            "avg_win, avg_loss, max_win, max_loss, avg_holding_period, var_95, cvar_95, "
            "beta, correlation, downside_volatility) VALUES (";

        // Add parameters
        query += txn.quote(run_id) + ", ";
        query += "'" + format_timestamp(start_date) + "', ";
        query += "'" + format_timestamp(end_date) + "', ";

        // Add metrics in expected order
        const std::vector<std::string> metric_names = {
            "total_return", "sharpe_ratio", "sortino_ratio", "max_drawdown",
            "calmar_ratio", "volatility", "total_trades", "win_rate",
            "profit_factor", "avg_win", "avg_loss", "max_win", "max_loss",
            "avg_holding_period", "var_95", "cvar_95", "beta",
            "correlation", "downside_volatility"
        };

        for (size_t i = 0; i < metric_names.size(); ++i) {
            if (i > 0) query += ", ";
            auto it = metrics.find(metric_names[i]);
            if (it != metrics.end()) {
                query += std::to_string(it->second);
            } else {
                query += "0.0";  // Default value if metric not provided
            }
        }
        query += ")";

        txn.exec(query);
        txn.commit();

        INFO("Stored backtest summary results for run_id: " + run_id);

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store backtest summary: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_backtest_equity_curve_batch(
    const std::string& run_id,
    const std::vector<std::pair<Timestamp, double>>& equity_points,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    if (equity_points.empty()) {
        return Result<void>();
    }

    try {
        pqxx::work txn(*connection_);

        // Build batch INSERT query
        std::string query = "INSERT INTO " + table_name +
                           " (run_id, timestamp, equity) VALUES ";

        for (size_t i = 0; i < equity_points.size(); ++i) {
            if (i > 0) query += ", ";
            query += "(" + txn.quote(run_id) + ", '" +
                     format_timestamp(equity_points[i].first) + "', " +
                     std::to_string(equity_points[i].second) + ")";
        }

        txn.exec(query);
        txn.commit();

        INFO("Stored " + std::to_string(equity_points.size()) +
             " equity curve points for run_id: " + run_id);

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store backtest equity curve: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_backtest_positions(
    const std::vector<Position>& positions,
    const std::string& run_id,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    if (positions.empty()) {
        return Result<void>();
    }

    try {
        pqxx::work txn(*connection_);

        // Build batch INSERT query
        std::string query = "INSERT INTO " + table_name +
                           " (run_id, symbol, quantity, average_price) VALUES ";

        bool first = true;
        for (const auto& pos : positions) {
            // Skip zero positions
            if (std::abs(static_cast<double>(pos.quantity)) < 1e-10) {
                continue;
            }

            if (!first) query += ", ";
            first = false;

            query += "(" + txn.quote(run_id) + ", " +
                     txn.quote(pos.symbol) + ", " +
                     std::to_string(static_cast<double>(pos.quantity)) + ", " +
                     std::to_string(static_cast<double>(pos.average_price)) + ")";
        }

        if (!first) {  // Only execute if we have non-zero positions
            txn.exec(query);
            txn.commit();

            INFO("Stored " + std::to_string(positions.size()) +
                 " final positions for run_id: " + run_id);
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store backtest positions: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::update_live_results(
    const std::string& strategy_id,
    const Timestamp& date,
    const std::unordered_map<std::string, double>& updates,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    // Validate strategy ID
    auto strategy_validation = validate_strategy_id(strategy_id);
    if (strategy_validation.is_error()) {
        return strategy_validation;
    }

    if (updates.empty()) {
        return Result<void>();
    }

    try {
        pqxx::work txn(*connection_);

        // Build UPDATE query
        std::string query = "UPDATE " + table_name + " SET ";

        bool first = true;
        for (const auto& [column, value] : updates) {
            if (!first) query += ", ";
            query += column + " = " + std::to_string(value);
            first = false;
        }

        query += " WHERE strategy_id = " + txn.quote(strategy_id) +
                " AND DATE(date) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
        txn.commit();

        INFO("Updated live results for " + strategy_id + " on " + format_timestamp(date) +
             " (" + std::to_string(result.affected_rows()) + " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to update live results: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::update_live_equity_curve(
    const std::string& strategy_id,
    const Timestamp& date,
    double equity,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    // Validate strategy ID
    auto strategy_validation = validate_strategy_id(strategy_id);
    if (strategy_validation.is_error()) {
        return strategy_validation;
    }

    try {
        pqxx::work txn(*connection_);

        std::string query = "UPDATE " + table_name +
                           " SET equity = " + std::to_string(equity) +
                           " WHERE strategy_id = " + txn.quote(strategy_id) +
                           " AND DATE(timestamp) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
        txn.commit();

        INFO("Updated equity curve for " + strategy_id + " on " + format_timestamp(date) +
             " (" + std::to_string(result.affected_rows()) + " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to update equity curve: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::delete_live_results(
    const std::string& strategy_id,
    const Timestamp& date,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    // Validate strategy ID
    auto strategy_validation = validate_strategy_id(strategy_id);
    if (strategy_validation.is_error()) {
        return strategy_validation;
    }

    try {
        pqxx::work txn(*connection_);

        std::string query = "DELETE FROM " + table_name +
                           " WHERE strategy_id = " + txn.quote(strategy_id) +
                           " AND DATE(date) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
        txn.commit();

        INFO("Deleted live results for " + strategy_id + " on " + format_timestamp(date) +
             " (" + std::to_string(result.affected_rows()) + " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete live results: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::delete_live_equity_curve(
    const std::string& strategy_id,
    const Timestamp& date,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    // Validate strategy ID
    auto strategy_validation = validate_strategy_id(strategy_id);
    if (strategy_validation.is_error()) {
        return strategy_validation;
    }

    try {
        pqxx::work txn(*connection_);

        std::string query = "DELETE FROM " + table_name +
                           " WHERE strategy_id = " + txn.quote(strategy_id) +
                           " AND DATE(timestamp) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
        txn.commit();

        INFO("Deleted equity curve for " + strategy_id + " on " + format_timestamp(date) +
             " (" + std::to_string(result.affected_rows()) + " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete equity curve: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_live_results_complete(
    const std::string& strategy_id,
    const Timestamp& date,
    const std::unordered_map<std::string, double>& metrics,
    const std::unordered_map<std::string, int>& int_metrics,
    const nlohmann::json& config,
    const std::string& table_name) {

    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    // Validate table name
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    // Validate strategy ID
    auto strategy_validation = validate_strategy_id(strategy_id);
    if (strategy_validation.is_error()) {
        return strategy_validation;
    }

    try {
        pqxx::work txn(*connection_);

        // Build column list and values
        std::string columns = "strategy_id, date";
        std::string values = txn.quote(strategy_id) + ", '" + format_timestamp(date) + "'";

        // Add double metrics
        for (const auto& [column, value] : metrics) {
            columns += ", " + column;
            values += ", " + std::to_string(value);
        }

        // Add integer metrics
        for (const auto& [column, value] : int_metrics) {
            columns += ", " + column;
            values += ", " + std::to_string(value);
        }

        // Add config as JSON
        if (!config.is_null()) {
            columns += ", config";
            values += ", " + txn.quote(config.dump());
        }

        std::string query = "INSERT INTO " + table_name +
                           " (" + columns + ") VALUES (" + values + ")";

        txn.exec(query);
        txn.commit();

        INFO("Stored complete live results for " + strategy_id + " on " + format_timestamp(date));

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store live results: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

}  // namespace trade_ngin