// src/data/postgres_database_extensions.cpp
// Phase 0: Database Extensions to Replace Raw SQL
// This file contains new methods to eliminate raw SQL from backtest and live trading

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <sstream>
#include <iomanip>
#include <ctime>

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
    const std::string& portfolio_id,
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

        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;
        
        // Build INSERT query with all metrics
        std::string query = "INSERT INTO " + table_name + " ("
            "run_id, portfolio_id, start_date, end_date, total_return, sharpe_ratio, sortino_ratio, "
            "max_drawdown, calmar_ratio, volatility, total_trades, win_rate, profit_factor, "
            "avg_win, avg_loss, max_win, max_loss, avg_holding_period, var_95, cvar_95, "
            "beta, correlation, downside_volatility) VALUES (";

        // Add parameters
        query += txn.quote(run_id) + ", ";
        query += txn.quote(actual_portfolio_id) + ", ";
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
    const std::string& portfolio_id,
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

        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;
        
        // Build batch INSERT query
        std::string query = "INSERT INTO " + table_name +
                           " (run_id, portfolio_id, timestamp, equity) VALUES ";

        for (size_t i = 0; i < equity_points.size(); ++i) {
            if (i > 0) query += ", ";
            query += "(" + txn.quote(run_id) + ", " + txn.quote(actual_portfolio_id) + ", '" +
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
    const std::string& portfolio_id,
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

        // Get the date from the first position (all positions should be from the same date)
        // Extract date from last_update timestamp
        auto time_t = std::chrono::system_clock::to_time_t(positions[0].last_update);
        std::stringstream date_ss;
        std::tm time_info;
        trade_ngin::core::safe_gmtime(&time_t, &time_info);
        date_ss << std::put_time(&time_info, "%Y-%m-%d");
        std::string position_date = date_ss.str();

        // Extract actual_run_id and strategy_id from composite run_id
        // Format: "backtest_run_id|strategy_id" OR just "run_id" for legacy
        std::string actual_run_id_for_delete = run_id;
        std::string strategy_id_for_delete = run_id;
        
        size_t pipe_pos = run_id.find('|');
        if (pipe_pos != std::string::npos) {
            actual_run_id_for_delete = run_id.substr(0, pipe_pos);
            strategy_id_for_delete = run_id.substr(pipe_pos + 1);
        }

        // Clear existing positions for this run_id, strategy_id, and date
        // This allows storing positions daily without duplicates
        try {
            std::string delete_query = "DELETE FROM " + table_name + 
                " WHERE run_id = " + txn.quote(actual_run_id_for_delete) + 
                " AND strategy_id = " + txn.quote(strategy_id_for_delete) +
                " AND DATE(date) = '" + position_date + "'";
            txn.exec(delete_query);
        } catch (const std::exception& e) {
            // If date column doesn't exist yet (old schema), try without it
            WARN("date column may not exist, trying delete without date: " + std::string(e.what()));
            try {
                std::string delete_query = "DELETE FROM " + table_name + 
                    " WHERE run_id = " + txn.quote(actual_run_id_for_delete) + 
                    " AND strategy_id = " + txn.quote(strategy_id_for_delete) +
                    " AND DATE(last_update) = '" + position_date + "'";
                txn.exec(delete_query);
            } catch (const std::exception& e2) {
                // If last_update doesn't exist either, skip delete (old schema)
                WARN("Could not delete existing positions, continuing with insert: " + std::string(e2.what()));
            }
        }

        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;
        
        // Build batch INSERT query with all required columns for daily storage
        // Try new schema first (with date, last_update, unrealized_pnl, realized_pnl)
        std::string query = "INSERT INTO " + table_name +
                           " (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at) VALUES ";

        bool first = true;
        std::vector<std::string> position_values;
        
        for (const auto& pos : positions) {
            // Skip zero positions
            if (std::abs(static_cast<double>(pos.quantity)) < 1e-10) {
                continue;
            }

            // Extract date from last_update timestamp
            auto pos_time_t = std::chrono::system_clock::to_time_t(pos.last_update);
            std::stringstream pos_date_ss;
            std::tm pos_time_info;
            trade_ngin::core::safe_gmtime(&pos_time_t, &pos_time_info);
            pos_date_ss << std::put_time(&pos_time_info, "%Y-%m-%d");
            std::string pos_date_str = pos_date_ss.str();

            // Format timestamps
            std::string last_update_str = format_timestamp(pos.last_update);
            
            // Extract strategy_id from run_id if it contains a pipe separator
            // Format: "backtest_run_id|strategy_id" OR just "strategy_id" for legacy
            std::string actual_run_id = run_id;
            std::string strategy_id_for_db = run_id;  // Default to run_id
            
            size_t pipe_pos = run_id.find('|');
            if (pipe_pos != std::string::npos) {
                // New format: backtest_run_id|strategy_id
                actual_run_id = run_id.substr(0, pipe_pos);
                strategy_id_for_db = run_id.substr(pipe_pos + 1);
            }
            
            std::stringstream value_ss;
            value_ss << "(" << txn.quote(actual_run_id) << ", "
                     << txn.quote(actual_portfolio_id) << ", "
                     << txn.quote(strategy_id_for_db) << ", "
                     << "'" << pos_date_str << "', "
                     << txn.quote(pos.symbol) << ", "
                     << std::to_string(static_cast<double>(pos.quantity)) << ", "
                     << std::to_string(static_cast<double>(pos.average_price)) << ", "
                     << std::to_string(static_cast<double>(pos.unrealized_pnl)) << ", "
                     << std::to_string(static_cast<double>(pos.realized_pnl)) << ", "
                     << "'" << last_update_str << "', "
                     << "'" << last_update_str << "'"
                     << ")";
            
            position_values.push_back(value_ss.str());
        }

        if (!position_values.empty()) {
            // Try new schema first
            try {
                // Join position values
                bool first_val = true;
                for (const auto& val : position_values) {
                    if (!first_val) query += ", ";
                    first_val = false;
                    query += val;
                }
                
                DEBUG("Executing position insert query for run_id: " + run_id + ", date: " + position_date);
                DEBUG("Query: " + query.substr(0, 200) + "...");  // Log first 200 chars
                
                txn.exec(query);
                txn.commit();

                INFO("Successfully stored " + std::to_string(position_values.size()) +
                     " positions for run_id: " + run_id + " on date: " + position_date);
            } catch (const std::exception& e) {
                // Log the actual error for debugging
                ERROR("Failed to insert positions with new schema: " + std::string(e.what()));
                ERROR("run_id: " + run_id + ", date: " + position_date);
                if (query.length() > 1000) {
                    ERROR("Query (first 1000 chars): " + query.substr(0, 1000));
                } else {
                    ERROR("Full query: " + query);
                }
                txn.abort();
                
                // Don't fallback to old schema if date column exists (it's required)
                // The error should be fixed, not worked around
                return make_error<void>(ErrorCode::DATABASE_ERROR, 
                                       "Failed to store positions: " + std::string(e.what()), 
                                       component_id_);
            }
        } else {
            DEBUG("No position values to insert (all positions were zero or empty)");
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store backtest positions: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_backtest_positions_with_strategy(
    const std::vector<Position>& positions,
    const std::string& run_id,
    const std::string& strategy_id,
    const std::string& portfolio_id,
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

        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;
        
        // Build batch INSERT query with strategy_id and all required columns
        // Schema requires: run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, 
        //                  unrealized_pnl, realized_pnl, last_update, updated_at
        std::string query = "INSERT INTO " + table_name +
                           " (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at) VALUES ";

        bool first = true;
        for (const auto& pos : positions) {
            // Skip zero positions
            if (std::abs(static_cast<double>(pos.quantity)) < 1e-10) {
                continue;
            }

            if (!first) query += ", ";
            first = false;

            // Extract date from last_update timestamp (thread-safe)
            auto time_t = std::chrono::system_clock::to_time_t(pos.last_update);
            std::stringstream date_ss;
            std::tm time_info;
            trade_ngin::core::safe_gmtime(&time_t, &time_info);
            date_ss << std::put_time(&time_info, "%Y-%m-%d");
            std::string date_str = date_ss.str();

            // Format timestamps using member function
            std::string last_update_str = format_timestamp(pos.last_update);
            
            query += "(" + txn.quote(run_id) + ", " +
                     txn.quote(actual_portfolio_id) + ", " +
                     txn.quote(strategy_id) + ", " +
                     "'" + date_str + "', " +  // date column
                     txn.quote(pos.symbol) + ", " +
                     std::to_string(static_cast<double>(pos.quantity)) + ", " +
                     std::to_string(static_cast<double>(pos.average_price)) + ", " +
                     std::to_string(static_cast<double>(pos.realized_pnl)) + ", " +
                     std::to_string(static_cast<double>(pos.unrealized_pnl)) + ", " +
                     "'" + last_update_str + "', " +  // last_update
                     "'" + last_update_str + "'" +    // updated_at (same as last_update)
                     ")";
        }

        if (!first) {  // Only execute if we have non-zero positions
            txn.exec(query);
            txn.commit();

            INFO("Stored " + std::to_string(positions.size()) +
                 " final positions for run_id: " + run_id + ", strategy_id: " + strategy_id);
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store backtest positions with strategy: " + std::string(e.what()));
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