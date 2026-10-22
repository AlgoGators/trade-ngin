// src/data/postgres_database_extensions.cpp
// Phase 0: Database Extensions to Replace Raw SQL
// This file contains new methods to eliminate raw SQL from backtest and live trading

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/backtest_position_row.hpp"
#include "trade_ngin/data/finite_statement.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

// ============================================================================
// NEW METHODS TO REPLACE RAW SQL (Phase 0 Refactoring)
// ============================================================================

Result<void> PostgresDatabase::delete_stale_executions(const std::vector<std::string>& order_ids,
                                                       const Timestamp& date,
                                                       const std::string& strategy_name,
                                                       const std::string& portfolio_id,
                                                       const std::string& table_name,
                                                       const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021

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
            if (i > 0)
                in_list += ", ";
            in_list += txn.quote(order_ids[i]);
        }

        // E2-F4: scoped by portfolio_id. Without it this reaches across books -- order_id
        // is portfolio-independent and TREND_FOLLOWING runs in both. See the header.
        // F-C: no calendar-date predicate. order_id is deterministic and already carries
        // the trading date (DAILY_<symbol>_<yyyymmdd>, CORPACTION_<symbol>_<ex_date>), while
        // execution_time is a wall-clock instant: a run at 19:00 EDT stores Monday UTC and a
        // re-run at 21:00 EDT asked for Tuesday UTC, so the first run's rows never matched
        // and the re-run inserted duplicates. Scoping by portfolio and strategy_name stays.
        // Migration 021: and by book. Order ids are the same in every book (the system ids are
        // not changed), so without it a desk book's cleanup would take the system rows.
        (void)date;
        std::string query = "DELETE FROM " + table_name +
                            " WHERE strategy_name = $1 "
                            " AND portfolio_id = $2 "
                            " AND portfolio_type = $3 "
                            " AND order_id IN (" +
                            in_list + ")";

        txn.exec(query, pqxx::params{strategy_name, portfolio_id, book});

        txn.commit();

        INFO("Deleted stale executions for " + std::to_string(order_ids.size()) + " order IDs on " +
             format_timestamp(date) + " for strategy " + strategy_name);

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete stale executions: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::delete_roll_executions(const Timestamp& date,
                                                      const std::string& strategy_name,
                                                      const std::string& portfolio_id,
                                                      const std::string& table_name,
                                                      const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021
    auto validation = validate_connection();
    if (validation.is_error()) return validation;
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) return table_validation;
    try {
        pqxx::work txn(*connection_);
        const std::string day = trade_ngin::core::format_utc_date(date);
        const auto r = txn.exec("DELETE FROM " + table_name +
                                    " WHERE strategy_name = $1 AND portfolio_id = $2 AND date = $3::date"
                                    " AND execution_type = 'ROLL' AND portfolio_type = $4",
                                pqxx::params{strategy_name, portfolio_id, day, book});
        txn.commit();
        (void)r;  // the sweep writes no log line: its rows are re-stored right after (section 7)
        return Result<void>();
    } catch (const std::exception& e) {
        ERROR("ROLL_LEG STOP: the day's ROLL executions could not be deleted: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<double> PostgresDatabase::get_previous_total_roll_costs(const std::string& strategy_id,
                                                               const std::string& portfolio_id,
                                                               const Timestamp& date,
                                                               const std::string& table_name,
                                                               const std::string& executions_table,
                                                               const std::string& book) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<double>(validation.error()->code(), validation.error()->what());
    }
    if (auto bv = validate_book(book); bv.is_error()) {
        return make_error<double>(bv.error()->code(), bv.error()->what(), "PostgresDatabase");
    }
    try {
        pqxx::work txn(*connection_);
        for (const auto& name : {table_name, executions_table}) {
            auto table_validation = validate_table_name(name);
            if (table_validation.is_error()) {
                return make_error<double>(table_validation.error()->code(),
                                          table_validation.error()->what());
            }
        }
        const std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;
        // The latest stored row before the date, plus the ROLL legs stored after that row's date
        // and before the date (days whose live_results write failed).
        auto result = txn.exec(
            "WITH prev AS (SELECT DATE(date) AS d, COALESCE(total_roll_costs, 0) AS t FROM " +
                table_name +
                " WHERE strategy_id = $1 AND portfolio_id = $2 AND DATE(date) < DATE($3)"
                " AND portfolio_type = $4 ORDER BY date DESC, created_at DESC LIMIT 1)"
                " SELECT COALESCE((SELECT t FROM prev), 0) + COALESCE((SELECT SUM(total_transaction_costs)"
                " FROM " + executions_table +
                " WHERE strategy_id = $1 AND portfolio_id = $2 AND execution_type = 'ROLL'"
                " AND portfolio_type = $4"
                " AND date < DATE($3) AND date > COALESCE((SELECT d FROM prev), DATE '-infinity')), 0)",
            pqxx::params{strategy_id, actual_portfolio_id, format_timestamp(date), book});
        txn.commit();
        if (result.empty() || result[0][0].is_null()) return Result<double>(0.0);
        return Result<double>(result[0][0].as<double>());
    } catch (const std::exception& e) {
        return make_error<double>(ErrorCode::DATABASE_ERROR,
                                  "Failed to read the previous total_roll_costs: " + std::string(e.what()),
                                  "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, std::string>> PostgresDatabase::get_stored_roll_contracts(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date,
    const std::string& table_name, const std::string& book) {
    using Contracts = std::unordered_map<std::string, std::string>;
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Contracts>(validation.error()->code(), validation.error()->what());
    }
    if (auto bv = validate_book(book); bv.is_error()) {
        return make_error<Contracts>(bv.error()->code(), bv.error()->what(), "PostgresDatabase");
    }
    try {
        pqxx::work txn(*connection_);
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return make_error<Contracts>(table_validation.error()->code(),
                                         table_validation.error()->what());
        }
        const std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;
        // The exec id carries the confirming bar's date, so the first row per symbol is its
        // earliest roll of the day: the contract the book held before the day's legs.
        auto result = txn.exec(
            "SELECT DISTINCT ON (symbol) symbol, COALESCE(instrument_id, '') FROM " + table_name +
                " WHERE strategy_id = $1 AND portfolio_id = $2 AND date = $3::date"
                " AND portfolio_type = $4"
                " AND execution_type = 'ROLL' AND exec_id LIKE '%\\_RC' ORDER BY symbol, exec_id",
            pqxx::params{strategy_id, actual_portfolio_id, trade_ngin::core::format_utc_date(date),
                         book});
        txn.commit();
        Contracts out;
        for (const auto& row : result) out[row[0].as<std::string>()] = row[1].as<std::string>();
        return Result<Contracts>(out);
    } catch (const std::exception& e) {
        return make_error<Contracts>(ErrorCode::DATABASE_ERROR,
                                     "Failed to read the day's stored ROLL legs: " + std::string(e.what()),
                                     "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_summary(
    const std::string& run_id, const Timestamp& start_date, const Timestamp& end_date,
    const std::unordered_map<std::string, double>& metrics, const std::string& portfolio_id,
    const std::string& table_name, const std::vector<LiveResultsCell>& cells) {
    std::lock_guard<std::mutex> lock(mutex_);

    // A cell's column and type are concatenated into the statement: allow-listed before
    // anything else happens.
    auto cells_validation = validate_live_results_cells(cells);
    if (cells_validation.is_error()) {
        return cells_validation;
    }

    // A NOT NULL metric (migration 018) that is not finite refuses the row: no statement is
    // sent and the error is returned, as when the statement failed on it, so no 0 is stored in
    // its place (T-8D R12).
    if (const std::string refusal = non_finite_not_null_refusal(
            table_name + " INSERT for run_id " + run_id, metrics,
            backtest_results_not_null_metrics());
        !refusal.empty()) {
        ERROR(refusal);
        return make_error<void>(ErrorCode::DATABASE_ERROR, refusal, component_id_);
    }

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

        // The typed cells (migration 037): named only when they have a value, so a cell without
        // one stays NULL. The value travels quoted, at the precision its text carries, and is
        // cast by the statement.
        std::string cell_columns;
        std::string cell_values;
        for (const auto& cell : cells) {
            if (!cell.value) continue;
            cell_columns += ", " + cell.column;
            cell_values += ", " + txn.quote(*cell.value) + "::" + cell.type;
        }

        // Build INSERT query with all metrics
        std::string query =
            "INSERT INTO " + table_name +
            " ("
            "run_id, portfolio_id, start_date, end_date, total_return, sharpe_ratio, "
            "sortino_ratio, "
            "max_drawdown, calmar_ratio, volatility, total_trades, win_rate, profit_factor, "
            "avg_win, avg_loss, max_win, max_loss, avg_holding_period, var_95, cvar_95, "
            "beta, correlation, downside_volatility, transaction_costs, roll_costs, "
            "total_roll_fills" + cell_columns + ") VALUES (";

        // Add parameters
        query += txn.quote(run_id) + ", ";
        query += txn.quote(actual_portfolio_id) + ", ";
        query += "'" + format_timestamp(start_date) + "', ";
        query += "'" + format_timestamp(end_date) + "', ";

        // Add metrics in expected order
        const std::vector<std::string> metric_names = {
            "total_return", "sharpe_ratio", "sortino_ratio", "max_drawdown",       "calmar_ratio",
            "volatility",   "total_trades", "win_rate",      "profit_factor",      "avg_win",
            "avg_loss",     "max_win",      "max_loss",      "avg_holding_period", "var_95",
            "cvar_95",      "beta",         "correlation",   "downside_volatility",
            "transaction_costs", "roll_costs", "total_roll_fills"};  // 018 (T-ROLLX)

        // A metric that is not handed in or is not finite has no value: its cell is written
        // DEFAULT and the run is kept (T-8D R12, T-8D-2 R54). DEFAULT is NULL on the nullable
        // columns; a NOT NULL column that is not handed in takes its 0, as it did before the
        // guard (one that is not finite was refused above).
        std::vector<std::string> without_value;
        query += backtest_summary_metric_values(metric_names, metrics, &without_value);
        query += cell_values + ")";
        std::vector<std::string> stored_null;
        std::vector<std::string> stored_zero;
        for (const auto& name : without_value) {
            const auto& not_null = backtest_results_not_null_metrics();
            const bool is_not_null =
                std::find(not_null.begin(), not_null.end(), name) != not_null.end();
            (is_not_null ? stored_zero : stored_null).push_back(name);
        }
        if (!stored_null.empty()) {
            WARN("Backtest summary for run_id " + run_id + ": " +
                 std::to_string(stored_null.size()) +
                 " metric(s) without a finite value stored NULL: " +
                 joined_columns(stored_null));
        }
        if (!stored_zero.empty()) {
            WARN("Backtest summary for run_id " + run_id + ": " +
                 std::to_string(stored_zero.size()) +
                 " metric(s) not handed in, stored as the column default 0: " +
                 joined_columns(stored_zero));
        }

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
    const std::string& run_id, const std::vector<std::pair<Timestamp, double>>& equity_points,
    const std::string& portfolio_id, const std::string& table_name,
    const std::vector<std::string>& risk_detail, const BacktestCurveCells& cells) {
    std::lock_guard<std::mutex> lock(mutex_);

    // The typed cells (migration 037) carry one list per point or none at all, every list the
    // same columns: refused otherwise, as a risk_detail of another length is. A cell's column
    // is concatenated into the statement, so it is allow-listed before anything else happens.
    if (const std::string refusal = backtest_curve_cells_refusal(cells, equity_points.size());
        !refusal.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "store_backtest_equity_curve_batch: " + refusal,
                                "PostgresDatabase");
    }
    if (!cells.empty()) {
        auto cells_validation = validate_live_results_cells(cells.front());
        if (cells_validation.is_error()) {
            return cells_validation;
        }
    }

    // risk_detail (migration 020) carries one entry per point (a futures book) or none at all (the
    // equity book). Any other length would write the curve without the column and lose the day
    // records without a word: refused.
    if (!risk_detail.empty() && risk_detail.size() != equity_points.size()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "store_backtest_equity_curve_batch: " +
                                    std::to_string(risk_detail.size()) +
                                    " risk_detail entries for " +
                                    std::to_string(equity_points.size()) +
                                    " equity points; one per point or none",
                                "PostgresDatabase");
    }

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

        // Build batch INSERT query. risk_detail is named only when the caller carries one entry
        // per point; an empty entry is a NULL cell. A caller that passes none (the equity book)
        // writes the statement it always wrote.
        const bool with_detail = !risk_detail.empty();
        std::string query = "INSERT INTO " + table_name +
                            (with_detail ? " (run_id, portfolio_id, timestamp, equity, risk_detail"
                                         : " (run_id, portfolio_id, timestamp, equity") +
                            backtest_curve_cell_columns(cells) + ") VALUES ";
        const auto quote = [&txn](const std::string& text) { return txn.quote(text); };

        for (size_t i = 0; i < equity_points.size(); ++i) {
            if (i > 0)
                query += ", ";
            query += "(" + txn.quote(run_id) + ", " + txn.quote(actual_portfolio_id) + ", '" +
                     format_timestamp(equity_points[i].first) + "', " +
                     round_trip_number(equity_points[i].second);
            if (with_detail) {
                query += risk_detail[i].empty() ? std::string(", NULL")
                                                : ", " + txn.quote(risk_detail[i]) + "::jsonb";
            }
            if (!cells.empty()) {
                query += backtest_curve_cell_values(cells[i], quote);
            }
            query += ")";
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

Result<void> PostgresDatabase::store_backtest_positions(const std::vector<Position>& positions,
                                                        const std::string& run_id,
                                                        const std::string& portfolio_id,
                                                        const std::string& table_name,
                                                        bool keep_closed_rows,
                                                        const std::unordered_set<std::string>&
                                                            no_bar_symbols) {
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
                std::string delete_query =
                    "DELETE FROM " + table_name +
                    " WHERE run_id = " + txn.quote(actual_run_id_for_delete) +
                    " AND strategy_id = " + txn.quote(strategy_id_for_delete) +
                    " AND DATE(last_update) = '" + position_date + "'";
                txn.exec(delete_query);
            } catch (const std::exception& e2) {
                // If last_update doesn't exist either, skip delete (old schema)
                WARN("Could not delete existing positions, continuing with insert: " +
                     std::string(e2.what()));
            }
        }

        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // Build batch INSERT query with all required columns for daily storage
        // Try new schema first (with date, last_update, unrealized_pnl, realized_pnl)
        std::string query =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, "
            "unrealized_pnl, realized_pnl, last_update, updated_at, instrument_id, row_mark) "
            "VALUES ";

        [[maybe_unused]] bool first = true;
        std::vector<std::string> position_values;

        for (const auto& pos : positions) {
            // Skip zero positions. E2-F54: a cash book passes keep_closed_rows so that a
            // position closed to zero keeps the row carrying that bar's realized flow --
            // dropping it strands the exit's P&L. The rule is the live one
            // (LiveDailyCycle::is_dead_row): dead means no quantity AND no realized.
            // Futures leave the flag false and keep the original unconditional filter.
            if (!backtest_position_row_is_stored(static_cast<double>(pos.quantity),
                                                 static_cast<double>(pos.realized_pnl),
                                                 keep_closed_rows)) {
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
            value_ss << "(" << txn.quote(actual_run_id) << ", " << txn.quote(actual_portfolio_id)
                     << ", " << txn.quote(strategy_id_for_db) << ", "
                     << "'" << pos_date_str << "', " << txn.quote(pos.symbol) << ", "
                     << round_trip_number(static_cast<double>(pos.quantity)) << ", "
                     << round_trip_number(static_cast<double>(pos.average_price)) << ", "
                     << round_trip_number(static_cast<double>(pos.unrealized_pnl)) << ", "
                     << round_trip_number(static_cast<double>(pos.realized_pnl)) << ", "
                     << "'" << last_update_str << "', "
                     << "'" << last_update_str << "', "
                     << (pos.instrument_id.empty() ? std::string("NULL") : txn.quote(pos.instrument_id))
                     << ", "  // instrument_id (016)
                     << backtest_position_row_mark_value(no_bar_symbols.count(pos.symbol) > 0)
                     << ")";  // row_mark (038)

            position_values.push_back(value_ss.str());
        }

        if (!position_values.empty()) {
            // Try new schema first
            try {
                // Join position values
                bool first_val = true;
                for (const auto& val : position_values) {
                    if (!first_val)
                        query += ", ";
                    first_val = false;
                    query += val;
                }

                DEBUG("Executing position insert query for run_id: " + run_id +
                      ", date: " + position_date);
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

Result<void> PostgresDatabase::validate_live_results_cells(
    const std::vector<LiveResultsCell>& cells) const {
    // A cell's column and type are concatenated into the statement: the column is allow-listed
    // as an identifier and the type against the fixed list; the value is always quoted.
    for (const auto& cell : cells) {
        auto column_validation = validate_identifier(cell.column);
        if (column_validation.is_error()) {
            return column_validation;
        }
        if (!live_results_cell_type_known(cell.type)) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid live_results cell type: " + cell.type,
                                    "PostgresDatabase");
        }
    }
    return Result<void>();
}

Result<void> PostgresDatabase::update_live_results(
    const std::string& strategy_id, const Timestamp& date,
    const std::unordered_map<std::string, double>& updates, const std::string& portfolio_id,
    const std::string& table_name, size_t* rows_affected,
    const std::vector<LiveResultsCell>& cells, const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021

    if (rows_affected) {
        *rows_affected = 0;
    }

    // Column names are concatenated into the statement (Postgres cannot bind
    // identifiers), so allow-list every key before anything else happens.
    for (const auto& [column, value] : updates) {
        auto column_validation = validate_identifier(column);
        if (column_validation.is_error()) {
            return column_validation;
        }
    }
    auto cells_validation = validate_live_results_cells(cells);
    if (cells_validation.is_error()) {
        return cells_validation;
    }

    // A NOT NULL column (migration 017) that is not finite refuses the update: no statement is
    // sent, the row keeps every cell it had, and the error is returned, as when the statement
    // failed on it (T-8D R12).
    if (const std::string refusal = non_finite_not_null_refusal(
            table_name + " UPDATE for " + strategy_id + " on " + format_timestamp(date), updates,
            live_results_not_null_columns());
        !refusal.empty()) {
        ERROR(refusal);
        return make_error<void>(ErrorCode::DATABASE_ERROR, refusal, component_id_);
    }

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

    if (updates.empty() && cells.empty()) {
        return Result<void>();
    }

    try {
        pqxx::work txn(*connection_);

        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // Build UPDATE query
        std::string query = "UPDATE " + table_name + " SET ";

        // A value that is not finite is assigned NULL, never formatted into the statement
        // (T-8D R12); every column that reaches this is nullable (a NOT NULL one was refused
        // above).
        std::vector<std::string> without_value;
        query += live_results_update_assignments(updates, &without_value);
        bool first = updates.empty();
        if (!without_value.empty()) {
            WARN("Live results update for " + strategy_id + " on " + format_timestamp(date) +
                 ": " + std::to_string(without_value.size()) +
                 " value(s) not finite, stored NULL: " + joined_columns(without_value));
        }
        // The typed cells (migration 036): every one is assigned, NULL where it has no value.
        for (const auto& cell : cells) {
            if (!first)
                query += ", ";
            query += cell.column + " = " +
                     (cell.value ? txn.quote(*cell.value) + "::" + cell.type : std::string("NULL"));
            first = false;
        }

        // The key values are bound; only the allow-listed column names and the numbers above are
        // in the text. Migration 021: scoped to the book.
        query += " WHERE strategy_id = $1 AND portfolio_id = $2 AND DATE(date) = $3::date"
                 " AND portfolio_type = $4";

        auto result = txn.exec(query, pqxx::params{strategy_id, actual_portfolio_id,
                                                   format_timestamp(date).substr(0, 10), book});
        txn.commit();
        if (rows_affected) {
            *rows_affected = static_cast<size_t>(result.affected_rows());
        }

        INFO("Updated live results for " + strategy_id + " on " + format_timestamp(date) + " (" +
             std::to_string(result.affected_rows()) + " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to update live results: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::update_live_equity_curve(const std::string& strategy_id,
                                                        const Timestamp& date, double equity,
                                                        const std::string& portfolio_id,
                                                        const std::string& table_name,
                                                        const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021

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

        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // The value at round trip (ours); the key bound and scoped to the book (migration 021).
        std::string query = "UPDATE " + table_name + " SET equity = " + round_trip_number(equity) +
                            " WHERE strategy_id = $1 AND portfolio_id = $2"
                            " AND DATE(timestamp) = $3::date AND portfolio_type = $4";

        auto result = txn.exec(query, pqxx::params{strategy_id, actual_portfolio_id,
                                                   format_timestamp(date).substr(0, 10), book});
        txn.commit();

        INFO("Updated equity curve for " + strategy_id + " on " + format_timestamp(date) + " (" +
             std::to_string(result.affected_rows()) + " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to update equity curve: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::delete_live_results(const std::string& strategy_id,
                                                   const Timestamp& date,
                                                   const std::string& portfolio_id,
                                                   const std::string& table_name,
                                                   const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021

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

        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // Migration 021: scoped to the book, so a desk book's re-run never deletes the system row.
        std::string query = "DELETE FROM " + table_name +
                            " WHERE strategy_id = $1 AND portfolio_id = $2"
                            " AND DATE(date) = $3::date AND portfolio_type = $4";

        auto result = txn.exec(query, pqxx::params{strategy_id, actual_portfolio_id,
                                                   format_timestamp(date).substr(0, 10), book});
        txn.commit();

        INFO("Deleted live results for " + strategy_id + " (portfolio: " + actual_portfolio_id +
             ") on " + format_timestamp(date) + " (" + std::to_string(result.affected_rows()) +
             " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete live results: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::delete_live_equity_curve(const std::string& strategy_id,
                                                        const Timestamp& date,
                                                        const std::string& portfolio_id,
                                                        const std::string& table_name,
                                                        const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021

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

        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // Migration 021: scoped to the book (the stream), like the upsert that writes the row.
        std::string query = "DELETE FROM " + table_name +
                            " WHERE strategy_id = $1 AND portfolio_id = $2"
                            " AND DATE(timestamp) = $3::date AND portfolio_type = $4";

        auto result = txn.exec(query, pqxx::params{strategy_id, actual_portfolio_id,
                                                   format_timestamp(date).substr(0, 10), book});
        txn.commit();

        INFO("Deleted equity curve for " + strategy_id + " (portfolio: " + actual_portfolio_id +
             ") on " + format_timestamp(date) + " (" + std::to_string(result.affected_rows()) +
             " rows affected)");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete equity curve: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_live_results_complete(
    const std::string& strategy_id, const Timestamp& date,
    const std::unordered_map<std::string, double>& metrics,
    const std::unordered_map<std::string, int>& int_metrics, const nlohmann::json& config,
    const std::string& portfolio_id, const std::string& table_name,
    const nlohmann::json& risk_detail, const std::vector<LiveResultsCell>& cells,
    const std::string& book) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto bv = validate_book(book); bv.is_error()) return bv;  // migration 021

    // Column names are concatenated into the statement (Postgres cannot bind
    // identifiers), so allow-list every key before anything else happens.
    for (const auto& [column, value] : metrics) {
        auto column_validation = validate_identifier(column);
        if (column_validation.is_error()) {
            return column_validation;
        }
    }
    for (const auto& [column, value] : int_metrics) {
        auto column_validation = validate_identifier(column);
        if (column_validation.is_error()) {
            return column_validation;
        }
    }
    auto cells_validation = validate_live_results_cells(cells);
    if (cells_validation.is_error()) {
        return cells_validation;
    }

    // A NOT NULL column (migration 017) that is not finite refuses the row: no statement is
    // sent and the error is returned, as when the statement failed on it, so no 0 is stored in
    // its place (T-8D R12).
    if (const std::string refusal = non_finite_not_null_refusal(
            table_name + " INSERT for " + strategy_id + " on " + format_timestamp(date), metrics,
            live_results_not_null_columns());
        !refusal.empty()) {
        ERROR(refusal);
        return make_error<void>(ErrorCode::DATABASE_ERROR, refusal, component_id_);
    }

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

        // Use provided portfolio_id or default to BASE_PORTFOLIO
        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // Build column list and values - include portfolio_id
        // portfolio_type (migration 021): the book, validated above, always named.
        std::string columns = "strategy_id, portfolio_id, date, portfolio_type";
        std::string values = txn.quote(strategy_id) + ", " + txn.quote(actual_portfolio_id) +
                             ", '" + format_timestamp(date) + "', " + txn.quote(book);

        // Add double metrics; a value that is not finite is written NULL, by name, so that
        // its cell does not take a column default of 0 (T-8D R12); every column that reaches
        // this is nullable (a NOT NULL one was refused above).
        std::vector<std::string> without_value;
        append_live_results_double_columns(metrics, columns, values, &without_value);
        if (!without_value.empty()) {
            WARN("Live results for " + strategy_id + " on " + format_timestamp(date) + ": " +
                 std::to_string(without_value.size()) +
                 " value(s) not finite, stored NULL: " +
                 joined_columns(without_value));
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

        // risk_detail (migration 020): named only on a row that carries one (a futures row of
        // a sized rebalance); every other row leaves the cell NULL and its statement unchanged.
        if (!risk_detail.is_null()) {
            columns += ", risk_detail";
            values += ", " + txn.quote(risk_detail.dump()) + "::jsonb";
        }

        // The typed cells (migration 036): named only when they have a value, so a cell
        // without one stays NULL.
        for (const auto& cell : cells) {
            if (!cell.value) continue;
            columns += ", " + cell.column;
            values += ", " + txn.quote(*cell.value) + "::" + cell.type;
        }

        std::string query =
            "INSERT INTO " + table_name + " (" + columns + ") VALUES (" + values + ")";

        txn.exec(query);
        txn.commit();

        INFO("Stored complete live results for " + strategy_id +
             " (portfolio: " + actual_portfolio_id + ") on " + format_timestamp(date));

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store live results: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_live_run_metadata(
    const Timestamp& date, const std::string& strategy_id, const std::string& portfolio_id,
    const nlohmann::json& strategy_allocations, const nlohmann::json& portfolio_config,
    const nlohmann::json& strategy_configs, const std::string& table_name) {
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

        // Format date as YYYY-MM-DD
        std::string date_str = format_timestamp(date).substr(0, 10);

        std::string actual_portfolio_id = portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id;

        // Build INSERT with ON CONFLICT UPDATE
        // Unique constraint: (date, strategy_id, portfolio_id)
        std::string query = "INSERT INTO " + table_name +
                            " (date, strategy_id, portfolio_id, strategy_allocations, "
                            "portfolio_config, strategy_configs) "
                            "VALUES (" +
                            "'" + date_str + "', " + txn.quote(strategy_id) + ", " +
                            txn.quote(actual_portfolio_id) + ", " +
                            txn.quote(strategy_allocations.dump()) + "::jsonb, " +
                            txn.quote(portfolio_config.dump()) + "::jsonb, " +
                            txn.quote(strategy_configs.dump()) + "::jsonb) " +
                            "ON CONFLICT (date, strategy_id, portfolio_id) DO UPDATE SET " +
                            "strategy_allocations = EXCLUDED.strategy_allocations, " +
                            "portfolio_config = EXCLUDED.portfolio_config, " +
                            "strategy_configs = EXCLUDED.strategy_configs";

        txn.exec(query);
        txn.commit();

        INFO("Stored live run metadata for " + strategy_id + " on " + date_str +
             " (portfolio: " + actual_portfolio_id + ")");

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to store live run metadata: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

}  // namespace trade_ngin