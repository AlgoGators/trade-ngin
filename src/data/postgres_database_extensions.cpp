// src/data/postgres_database_extensions.cpp
// Phase 0: Database Extensions to Replace Raw SQL
// This file contains new methods to eliminate raw SQL from backtest and live trading

#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace {
// SQL identifiers (column/table names) can't be bound as query parameters -- only values
// can. When an identifier has to be built dynamically, whitelist its character set instead:
// letters, digits, underscore, must not start with a digit. This rejects quotes, semicolons,
// whitespace, and comment sequences outright, regardless of what the caller intended.
bool is_valid_sql_identifier(const std::string& name) {
    if (name.empty() || name.size() > 63 || std::isdigit(static_cast<unsigned char>(name[0]))) {
        return false;
    }
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
            return false;
        }
    }
    return true;
}

using Json = nlohmann::json;
constexpr std::size_t kInspectionMaxBytes = 2u * 1024u * 1024u;

bool exact_keys(const Json& object, std::initializer_list<const char*> names) {
    if (!object.is_object() || object.size() != names.size()) return false;
    for (const char* name : names) if (!object.contains(name)) return false;
    return true;
}

bool finite_json_number(const Json& value) {
    return value.is_number() && std::isfinite(value.get<double>());
}

bool inspection_fits_transport(pqxx::work& txn, const Json& sealed) {
    const std::string compact = sealed.dump();
    if (compact.size() > kInspectionMaxBytes) return false;
    const auto measured = txn.exec("SELECT octet_length(($1::jsonb)::text)",
                                   pqxx::params{compact});
    return measured[0][0].as<long long>() <=
           static_cast<long long>(kInspectionMaxBytes);
}

bool exact_float_integer(double floating, const Json& integer) {
    if (!std::isfinite(floating) || std::trunc(floating) != floating) return false;
    if (integer.is_number_unsigned()) {
        return floating >= 0.0 && floating < std::ldexp(1.0, 64) &&
               static_cast<std::uint64_t>(floating) == integer.get<std::uint64_t>();
    }
    return floating >= -std::ldexp(1.0, 63) &&
           floating < std::ldexp(1.0, 63) &&
           static_cast<std::int64_t>(floating) == integer.get<std::int64_t>();
}

bool exact_numeric_allocation_match(const Json& selected, const Json& stored) {
    if (!selected.is_number() || !stored.is_number()) return false;
    if (selected.is_number_float()) {
        const double value = selected.get<double>();
        if (stored.is_number_float())
            return std::isfinite(value) && std::isfinite(stored.get<double>()) &&
                   value == stored.get<double>();
        return exact_float_integer(value, stored);
    }
    if (stored.is_number_float()) return exact_float_integer(stored.get<double>(), selected);
    if (selected.is_number_unsigned()) {
        const auto value = selected.get<std::uint64_t>();
        if (stored.is_number_unsigned()) return value == stored.get<std::uint64_t>();
        const auto other = stored.get<std::int64_t>();
        return other >= 0 && value == static_cast<std::uint64_t>(other);
    }
    const auto value = selected.get<std::int64_t>();
    if (stored.is_number_unsigned())
        return value >= 0 && static_cast<std::uint64_t>(value) == stored.get<std::uint64_t>();
    return value == stored.get<std::int64_t>();
}

bool valid_ascii_token(const std::string& token) {
    if (token.empty() || token.size() > 128) return false;
    for (unsigned char ch : token) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) return false;
    }
    return true;
}

bool valid_strategy_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    for (unsigned char ch : id) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
    }
    return true;
}

bool valid_utc_timestamp(const Json& value) {
    if (!value.is_string()) return false;
    const auto& stamp = value.get_ref<const std::string&>();
    if (stamp.size() != 27 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != 'T' ||
        stamp[13] != ':' || stamp[16] != ':' || stamp[19] != '.' || stamp[26] != 'Z')
        return false;
    for (std::size_t i = 0; i < stamp.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == 19 || i == 26) continue;
        if (stamp[i] < '0' || stamp[i] > '9') return false;
    }
    const auto number = [&](std::size_t start, std::size_t width) {
        int result = 0;
        for (std::size_t i = start; i < start + width; ++i)
            result = result * 10 + stamp[i] - '0';
        return result;
    };
    const int year = number(0, 4);
    const int month = number(5, 2);
    const int day = number(8, 2);
    if (year < 1 || month < 1 || month > 12 ||
        number(11, 2) > 23 || number(14, 2) > 59 || number(17, 2) > 59)
        return false;
    constexpr int days_in_month[] = {0, 31, 28, 31, 30, 31, 30, 31,
                                     31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days_in_month[month] + (month == 2 && leap ? 1 : 0);
}

bool valid_trend_map(const Json& stage) {
    if (!exact_keys(stage, {"weight", "risk_target", "fx_rate", "idm",
        "max_symbol_concentration", "use_position_buffering", "carver_buffer_floor",
        "carver_buffer_position_factor", "ema_windows", "vol_lookback_short",
        "vol_lookback_long", "max_history_size", "fdm"})) return false;
    for (const char* key : {"weight", "risk_target", "fx_rate", "idm",
                            "max_symbol_concentration", "carver_buffer_floor",
                            "carver_buffer_position_factor"})
        if (!finite_json_number(stage.at(key))) return false;
    if (!stage.at("use_position_buffering").is_boolean() ||
        !stage.at("vol_lookback_short").is_number_integer() ||
        !stage.at("vol_lookback_long").is_number_integer()) return false;
    for (const char* key : {"vol_lookback_short", "vol_lookback_long"}) {
        const auto& value = stage.at(key);
        if (value.is_number_unsigned()) {
            if (value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                return false;
        } else if (value.get<std::int64_t>() < std::numeric_limits<int>::min() ||
                   value.get<std::int64_t>() > std::numeric_limits<int>::max()) return false;
    }
    const auto& capacity = stage.at("max_history_size");
    if (!capacity.is_number_integer()) return false;
    if (!capacity.is_number_unsigned() && capacity.get<std::int64_t>() < 0) return false;
    for (const char* key : {"ema_windows", "fdm"}) {
        const auto& pairs = stage.at(key);
        if (!pairs.is_array()) return false;
        for (const auto& pair : pairs) {
            if (!pair.is_array() || pair.size() != 2 || !pair[0].is_number_integer())
                return false;
            if (key[0] == 'e' ? !pair[1].is_number_integer() : !finite_json_number(pair[1]))
                return false;
            for (int index = 0; index < (key[0] == 'e' ? 2 : 1); ++index) {
                const auto& number = pair[index];
                if (number.is_number_unsigned()) {
                    if (number.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                        return false;
                } else if (number.get<std::int64_t>() < std::numeric_limits<int>::min() ||
                           number.get<std::int64_t>() > std::numeric_limits<int>::max()) return false;
            }
        }
    }
    return true;
}


bool valid_pending_capture(const Json& capture, const Json& strategy_allocations) {
    if (!exact_keys(capture, {"capture_schema_version", "captured_at", "status",
                              "reason", "supplied", "selected_trend"}) ||
        !capture.at("capture_schema_version").is_number_integer() ||
        capture.at("capture_schema_version") != 1 ||
        !valid_utc_timestamp(capture.at("captured_at"))) return false;
    if (capture.at("status") == "unavailable")
        return (capture.at("reason") == "projection_invalid" ||
                capture.at("reason") == "selected_stage_unavailable" ||
                capture.at("reason") == "capture_failed") &&
               capture.at("supplied").is_null() && capture.at("selected_trend").is_null();
    if (capture.at("status") != "available" || capture.at("reason") != "none" ||
        !trade_ngin::validate_live_config_projection_for_publication(capture.at("supplied")) ||
        !strategy_allocations.is_object()) return false;
    std::map<std::string, const Json*> supplied_fields;
    for (const auto& field : capture.at("supplied").at("fields"))
        supplied_fields.emplace(field.at("path").get<std::string>(), &field);
    const auto& selected = capture.at("selected_trend");
    if (!exact_keys(selected, {"schema_version", "provenance",
                               "slow_concentration_override", "strategies"}) ||
        !selected.at("schema_version").is_number_integer() ||
        selected.at("schema_version") != 1 ||
        selected.at("provenance") != "shared_resolver_same_inputs" ||
        !selected.at("strategies").is_array() ||
        selected.at("strategies").empty() ||
        selected.at("strategies").size() != strategy_allocations.size()) return false;
    const auto& override = selected.at("slow_concentration_override");
    if (!(exact_keys(override, {"state"}) && override.at("state") == "absent") &&
        !(exact_keys(override, {"state", "value"}) && override.at("state") == "present" &&
          finite_json_number(override.at("value")))) return false;
    std::set<std::string> ids;
    for (const auto& row : selected.at("strategies")) {
        if (!exact_keys(row, {"strategy_id", "strategy_type", "selected_allocation",
                              "factory_resolved", "constructor_normalized"}) ||
             !row.at("strategy_id").is_string() ||
             !valid_strategy_id(row.at("strategy_id").get<std::string>()) ||
             !ids.insert(row.at("strategy_id").get<std::string>()).second) return false;
        const auto& id = row.at("strategy_id").get_ref<const std::string&>();
        const auto allocation = strategy_allocations.find(id);
        if (allocation == strategy_allocations.end() ||
            !exact_numeric_allocation_match(row.at("selected_allocation"), *allocation))
             return false;
        const auto& type = row.at("strategy_type");
        if (type != "TrendFollowingStrategy" && type != "TrendFollowingFastStrategy" &&
            type != "TrendFollowingSlowStrategy") return false;
        const auto prefix = "/strategies/" + row.at("strategy_id").get<std::string>() + "/";
        const auto enabled = supplied_fields.find(prefix + "enabled_live");
        const auto supplied_type = supplied_fields.find(prefix + "type");
        if (enabled == supplied_fields.end() || supplied_type == supplied_fields.end() ||
            enabled->second->at("value_state") != "included" ||
            enabled->second->at("value") != true) return false;
        if (supplied_type->second->at("value_state") == "included") {
            if (supplied_type->second->at("value") != type) return false;
        } else if (supplied_type->second->at("value_state") != "absent_in_input" ||
                   type != "TrendFollowingStrategy") return false;
        if (!valid_trend_map(row.at("factory_resolved")) ||
            !valid_trend_map(row.at("constructor_normalized"))) return false;
    }
    return true;
}
}  // namespace

namespace trade_ngin {

// ============================================================================
// NEW METHODS TO REPLACE RAW SQL (Phase 0 Refactoring)
// ============================================================================

Result<void> PostgresDatabase::delete_stale_executions(const std::vector<std::string>& order_ids,
                                                       const Timestamp& date,
                                                       const std::string& strategy_name,
                                                       const std::string& table_name, const std::string& portfolio_type) {
    if (portfolio_type == "qt_proposal") {
        poison_proposal_refusal();
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"proposal_execution_cleanup_unsupported");
    }
    if (!pending_publication_ || portfolio_type != "system")
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_unscoped_execution_cleanup_refused");
    return delete_stale_executions_scoped(order_ids,date,pending_publication_->strategy_id,
        strategy_name,pending_publication_->portfolio_id,table_name,portfolio_type);
}

Result<void> PostgresDatabase::delete_stale_executions_scoped(
    const std::vector<std::string>& order_ids, const Timestamp& date,
    const std::string& strategy_id, const std::string& strategy_name,
    const std::string& portfolio_id, const std::string& table_name,
    const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (strategy_id.empty() || strategy_name.empty() || portfolio_id.empty())
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"execution_cleanup_scope_required");
    if (defer_live_write(date,[this,order_ids,date,strategy_id,strategy_name,portfolio_id,table_name,portfolio_type]() {
        return delete_stale_executions_scoped(order_ids,date,strategy_id,strategy_name,portfolio_id,table_name,portfolio_type);
    })) return Result<void>();
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);

        // Build a safe IN (...) clause for the provided order_ids
        std::string in_list;
        in_list.reserve(order_ids.size() * 20);
        for (size_t i = 0; i < order_ids.size(); ++i) {
            if (i > 0)
                in_list += ", ";
            in_list += txn.quote(order_ids[i]);
        }

        std::string query = "DELETE FROM " + table_name +
                            " WHERE date = $1::date "
                            " AND strategy_name = $2 "
                            " AND strategy_id = $3 AND portfolio_id = $4 AND portfolio_type=$5 "
                            " AND order_id IN (" +
                            in_list + ")";

        // Execute delete for the specified date (YYYY-MM-DD)
        txn.exec(query, pqxx::params{format_timestamp(date).substr(0, 10), strategy_name,
                                    strategy_id,portfolio_id,portfolio_type});

        txn.commit();

        INFO("Deleted stale executions for " + std::to_string(order_ids.size()) + " order IDs on " +
             format_timestamp(date) + " for strategy " + strategy_name);

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Failed to delete stale executions: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_backtest_summary(
    const std::string& run_id, const Timestamp& start_date, const Timestamp& end_date,
    const std::unordered_map<std::string, double>& metrics, const std::string& portfolio_id,
    const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
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
        PublicationTransaction txn(*connection_, publication_transaction_);

        const std::string& actual_portfolio_id = portfolio_id;

        // Build INSERT query with all metrics
        std::string query =
            "INSERT INTO " + table_name +
            " ("
            "run_id, portfolio_id, start_date, end_date, total_return, sharpe_ratio, "
            "sortino_ratio, "
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
            "total_return", "sharpe_ratio", "sortino_ratio", "max_drawdown",       "calmar_ratio",
            "volatility",   "total_trades", "win_rate",      "profit_factor",      "avg_win",
            "avg_loss",     "max_win",      "max_loss",      "avg_holding_period", "var_95",
            "cvar_95",      "beta",         "correlation",   "downside_volatility"};

        for (size_t i = 0; i < metric_names.size(); ++i) {
            if (i > 0)
                query += ", ";
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
    const std::string& run_id, const std::vector<std::pair<Timestamp, double>>& equity_points,
    const std::string& portfolio_id, const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
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
        PublicationTransaction txn(*connection_, publication_transaction_);

        const std::string& actual_portfolio_id = portfolio_id;

        // Build batch INSERT query
        std::string query =
            "INSERT INTO " + table_name + " (run_id, portfolio_id, timestamp, equity) VALUES ";

        for (size_t i = 0; i < equity_points.size(); ++i) {
            if (i > 0)
                query += ", ";
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

Result<void> PostgresDatabase::store_backtest_positions(const std::vector<Position>& positions,
                                                        const std::string& run_id,
                                                        const std::string& portfolio_id,
                                                        const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
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
        PublicationTransaction txn(*connection_, publication_transaction_);

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

        const std::string& actual_portfolio_id = portfolio_id;

        // Build batch INSERT query with all required columns for daily storage
        // Try new schema first (with date, last_update, unrealized_pnl, realized_pnl)
        std::string query =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, "
            "unrealized_pnl, realized_pnl, last_update, updated_at) VALUES ";

        [[maybe_unused]] bool first = true;
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
            value_ss << "(" << txn.quote(actual_run_id) << ", " << txn.quote(actual_portfolio_id)
                     << ", " << txn.quote(strategy_id_for_db) << ", "
                     << "'" << pos_date_str << "', " << txn.quote(pos.symbol) << ", "
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

Result<void> PostgresDatabase::replace_backtest_positions_for_date(
    const std::vector<Position>& positions, const std::string& run_id,
    const std::string& strategy_id, const std::string& portfolio_id,
    const Timestamp& date, const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto validation = validate_connection();
    if (validation.is_error()) return validation;
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) return table_validation;
    if (run_id.empty() || strategy_id.empty() || portfolio_id.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Backtest position replacement requires non-empty run, strategy, and portfolio ids",
                                "PostgresDatabase");
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        const auto time = std::chrono::system_clock::to_time_t(date);
        std::tm utc{};
        trade_ngin::core::safe_gmtime(&time, &utc);
        std::ostringstream day_stream;
        day_stream << std::put_time(&utc, "%Y-%m-%d");
        const std::string day = day_stream.str();

        txn.exec("DELETE FROM " + table_name +
                     " WHERE run_id=$1 AND portfolio_id=$2 AND strategy_id=$3 AND date=$4::date",
                 pqxx::params{run_id, portfolio_id, strategy_id, day});

        const std::string insert =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, "
            "unrealized_pnl, realized_pnl, last_update, updated_at) "
            "VALUES ($1,$2,$3,$4::date,$5,$6,$7,$8,$9,$10::timestamp,$10::timestamp)";
        for (const auto& position : positions) {
            if (std::abs(position.quantity.as_double()) < 1e-10) continue;
            txn.exec(insert, pqxx::params{
                run_id, portfolio_id, strategy_id, day, position.symbol,
                position.quantity.as_double(), position.average_price.as_double(),
                position.unrealized_pnl.as_double(), position.realized_pnl.as_double(),
                format_timestamp(position.last_update)});
        }
        txn.commit();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to replace backtest positions: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_positions_with_strategy(
    const std::vector<Position>& positions, const std::string& run_id,
    const std::string& strategy_id, const std::string& portfolio_id,
    const std::string& table_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
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
        PublicationTransaction txn(*connection_, publication_transaction_);

        const std::string& actual_portfolio_id = portfolio_id;

        // Build batch INSERT query with strategy_id and all required columns
        // Schema requires: run_id, portfolio_id, strategy_id, date, symbol, quantity,
        // average_price,
        //                  unrealized_pnl, realized_pnl, last_update, updated_at
        std::string query =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, "
            "unrealized_pnl, realized_pnl, last_update, updated_at) VALUES ";

        bool first = true;
        for (const auto& pos : positions) {
            // Skip zero positions
            if (std::abs(static_cast<double>(pos.quantity)) < 1e-10) {
                continue;
            }

            if (!first)
                query += ", ";
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

            query += "(" + txn.quote(run_id) + ", " + txn.quote(actual_portfolio_id) + ", " +
                     txn.quote(strategy_id) + ", " + "'" + date_str + "', " +  // date column
                     txn.quote(pos.symbol) + ", " +
                     std::to_string(static_cast<double>(pos.quantity)) + ", " +
                     std::to_string(static_cast<double>(pos.average_price)) + ", " +
                     std::to_string(static_cast<double>(pos.realized_pnl)) + ", " +
                     std::to_string(static_cast<double>(pos.unrealized_pnl)) + ", " + "'" +
                     last_update_str + "', " +      // last_update
                     "'" + last_update_str + "'" +  // updated_at (same as last_update)
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
    const std::string& strategy_id, const Timestamp& date,
    const std::unordered_map<std::string, double>& updates, const std::string& portfolio_id,
    const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (defer_live_write(date,[this,strategy_id,date,updates,portfolio_id,table_name,portfolio_type]() {
        return update_live_results(strategy_id,date,updates,portfolio_id,table_name,portfolio_type);
    })) return Result<void>();
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        const std::string& actual_portfolio_id = portfolio_id;

        // Column names can't be bound as $n parameters -- only values can -- so every
        // column is checked against a strict identifier whitelist before it reaches the
        // query text. This is the only defense available for dynamic identifiers; every
        // value is still bound as a parameter below.
        for (const auto& [column, value] : updates) {
            (void)value;
            if (!is_valid_sql_identifier(column)) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Invalid column name in updates map: '" + column + "'", component_id_);
            }
        }

        std::string query = "UPDATE " + table_name + " SET ";
        pqxx::params params;
        int param_idx = 1;

        bool first = true;
        for (const auto& [column, value] : updates) {
            if (!first)
                query += ", ";
            query += column + " = $" + std::to_string(param_idx++);
            params.append(value);
            first = false;
        }

        // One increment per statement. These were three param_idx++ in a single
        // expression, whose operands C++ leaves unsequenced: the compiler may
        // number the placeholders in any order, and the three params below are
        // appended in a fixed one. Getting $n out of step with the values binds
        // the strategy id to the date predicate, so the UPDATE matches nothing
        // and yesterday's metrics are silently never finalised.
        const int strategy_param = param_idx++;
        const int portfolio_param = param_idx++;
        const int date_param = param_idx++;
        const int stream_param = param_idx++;
        query += " WHERE strategy_id = $" + std::to_string(strategy_param) +
                 " AND portfolio_id = $" + std::to_string(portfolio_param) +
                 " AND DATE(date) = $" + std::to_string(date_param) +
                 " AND portfolio_type = $" + std::to_string(stream_param);
        params.append(strategy_id);
        params.append(actual_portfolio_id);
        params.append(format_timestamp(date).substr(0, 10));
        params.append(portfolio_type);

        auto result = txn.exec(query, params);
        txn.commit();

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
                                                        const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (defer_live_write(date,[this,strategy_id,date,equity,portfolio_id,table_name,portfolio_type]() {
        return update_live_equity_curve(strategy_id,date,equity,portfolio_id,table_name,portfolio_type);
    })) return Result<void>();
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        const std::string& actual_portfolio_id = portfolio_id;

        std::string query = "UPDATE " + table_name + " SET equity = " + std::to_string(equity) +
                            " WHERE strategy_id = " + txn.quote(strategy_id) +
                            " AND portfolio_id = " + txn.quote(actual_portfolio_id) +
                            " AND portfolio_type = " + txn.quote(portfolio_type) +
                            " AND DATE(timestamp) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
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
                                                   const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (defer_live_write(date,[this,strategy_id,date,portfolio_id,table_name,portfolio_type]() {
        return delete_live_results(strategy_id,date,portfolio_id,table_name,portfolio_type);
    })) return Result<void>();
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        const std::string& actual_portfolio_id = portfolio_id;

        std::string query = "DELETE FROM " + table_name +
                            " WHERE strategy_id = " + txn.quote(strategy_id) +
                            " AND portfolio_id = " + txn.quote(actual_portfolio_id) +
                            " AND portfolio_type = " + txn.quote(portfolio_type) +
                            " AND DATE(date) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
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
                                                        const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (defer_live_write(date,[this,strategy_id,date,portfolio_id,table_name,portfolio_type]() {
        return delete_live_equity_curve(strategy_id,date,portfolio_id,table_name,portfolio_type);
    })) return Result<void>();
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Use actual portfolio_id or default to BASE_PORTFOLIO for backward compatibility
        const std::string& actual_portfolio_id = portfolio_id;

        std::string query = "DELETE FROM " + table_name +
                            " WHERE strategy_id = " + txn.quote(strategy_id) +
                            " AND portfolio_id = " + txn.quote(actual_portfolio_id) +
                            " AND portfolio_type = " + txn.quote(portfolio_type) +
                            " AND DATE(timestamp) = '" + format_timestamp(date).substr(0, 10) + "'";

        auto result = txn.exec(query);
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
    const std::string& portfolio_id, const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (defer_live_write(date,[this,strategy_id,date,metrics,int_metrics,config,portfolio_id,table_name,portfolio_type]() {
        return store_live_results_complete(strategy_id,date,metrics,int_metrics,config,portfolio_id,table_name,portfolio_type);
    }, ResultsPart)) return Result<void>();
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Use provided portfolio_id or default to BASE_PORTFOLIO
        const std::string& actual_portfolio_id = portfolio_id;

        // Column names can't be bound as $n parameters -- validate against the same
        // identifier whitelist as update_live_results before either map touches the query.
        for (const auto& [column, value] : metrics) {
            (void)value;
            if (!is_valid_sql_identifier(column)) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Invalid column name in metrics map: '" + column + "'", component_id_);
            }
        }
        for (const auto& [column, value] : int_metrics) {
            (void)value;
            if (!is_valid_sql_identifier(column)) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Invalid column name in int_metrics map: '" + column + "'", component_id_);
            }
        }

        // Build column list and parameter placeholders - include portfolio_id
        std::string columns = "strategy_id, portfolio_id, date, portfolio_type";
        std::string placeholders = "$1, $2, $3, $4";
        pqxx::params params;
        params.append(strategy_id);
        params.append(actual_portfolio_id);
        params.append(format_timestamp(date));
        params.append(portfolio_type);
        int param_idx = 5;

        // Add double metrics
        for (const auto& [column, value] : metrics) {
            columns += ", " + column;
            placeholders += ", $" + std::to_string(param_idx++);
            params.append(value);
        }

        // Add integer metrics
        for (const auto& [column, value] : int_metrics) {
            columns += ", " + column;
            placeholders += ", $" + std::to_string(param_idx++);
            params.append(value);
        }

        // Add config as JSON
        if (!config.is_null()) {
            columns += ", config";
            placeholders += ", $" + std::to_string(param_idx++);
            params.append(config.dump());
        }

        std::string query =
            "INSERT INTO " + table_name + " (" + columns + ") VALUES (" + placeholders + ")";

        txn.exec(query, params);
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
    const bool has_inspection = portfolio_config.is_object() &&
        portfolio_config.contains("config_inspection");
    if (pending_publication_ && !publication_transaction_ && !has_inspection &&
        pending_publication_->evidence_requirement ==
            PublicationEvidenceRequirement::RequiredFinalObservations) {
        pending_publication_->invalid_payload = true;
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "config_inspection_capture_required");
    }
    if (has_inspection && !publication_transaction_) {
        if (!pending_publication_)
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "config_inspection_requires_pending_publication");
        if (pending_publication_->inspection_capture_queued ||
            pending_publication_->strategy_id != strategy_id ||
            pending_publication_->portfolio_id != portfolio_id ||
            pending_publication_->date != format_timestamp(date).substr(0, 10)) {
            pending_publication_->invalid_payload = true;
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "config_inspection_publication_protocol_invalid");
        }
        if(strategy_id=="LIVE_EQUITY_MEAN_REVERSION") {
            const auto& capture=portfolio_config.at("config_inspection");
            if(!exact_keys(capture,{"capture_schema_version","captured_at"}) ||
                !capture.at("capture_schema_version").is_number_integer() ||
                capture.at("capture_schema_version")!=2 || !valid_utc_timestamp(capture.at("captured_at"))) {
                pending_publication_->invalid_payload=true;
                return make_error<void>(ErrorCode::INVALID_ARGUMENT,"equity_config_inspection_capture_invalid");
            }
        }
        pending_publication_->inspection_capture_queued = true;
    }
    if (defer_live_write(date,[this,date,strategy_id,portfolio_id,strategy_allocations,portfolio_config,strategy_configs,table_name]() {
        return store_live_run_metadata(date,strategy_id,portfolio_id,strategy_allocations,portfolio_config,strategy_configs,table_name);
    }, MetadataPart)) return Result<void>();
    if (has_inspection && (!pending_publication_ || !publication_transaction_))
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "config_inspection_publication_protocol_invalid");
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate connection
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
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
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id);

        nlohmann::json stored_portfolio_config = portfolio_config;
        if(has_inspection && strategy_id=="LIVE_EQUITY_MEAN_REVERSION") {
            const auto& pending=*pending_publication_;
            if(pending.evidence_requirement!=PublicationEvidenceRequirement::RequiredFinalObservations ||
                !pending.equity_final_consumption || !valid_ascii_token(pending.producer_version))
                throw std::runtime_error("equity_inspection_publication_incomplete");
            const auto observed=txn.exec("SELECT gen_random_uuid()::text, "
                "to_char(clock_timestamp() AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')");
            const auto recorded_at=observed[0][1].as<std::string>();
            const auto& publication_id=pending.publication_id;
            if(publication_id.empty())throw std::runtime_error("equity_inspection_publication_identity_missing");
            const auto& projected=pending.equity_final_consumption->document();
            const bool available=projected.at("available")==true && projected.at("complete")==true;
            nlohmann::json identity={{"registry_id",pending.registry_id},
                {"registry_revision",pending.registry_revision},{"engine_strategy_id",pending.strategy_id},
                {"portfolio_id",pending.portfolio_id},{"run_date",pending.date},
                {"capture_id",publication_id},{"publication_id",publication_id},
                {"runtime_attempt_id",pending.attempt_id.empty()?nlohmann::json(nullptr):nlohmann::json(pending.attempt_id)},
                {"producer_version",pending.producer_version},{"control_mode",pending.attempt_id.empty()?"uncontrolled":"controlled"}};
            stored_portfolio_config["config_inspection"]={{"publication_schema_version",3},
                {"profile","live_equity_mean_reversion"},{"authority","inspection_only"},{"stream","system"},
                {"identity",std::move(identity)},{"captured_at",portfolio_config.at("config_inspection").at("captured_at")},
                {"publication_recorded_at",recorded_at},{"status",available?"available":"unavailable"},
                {"reason",available?"none":"consumption_unavailable"},{"equity_run_consumption",projected}};
            if(!inspection_fits_transport(txn,stored_portfolio_config.at("config_inspection")))
                throw std::runtime_error("equity_inspection_capacity_exceeded");
        } else if (has_inspection) {
            // The scope was rechecked by publish_live_publication before this
            // callback. Identity and recorded time come from that transaction.
            auto observed = txn.exec(
                "SELECT gen_random_uuid()::text, "
                "to_char(clock_timestamp() AT TIME ZONE 'UTC', "
                "'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"')");
            const std::string recorded_at = observed[0][1].as<std::string>();
            const auto& pending = *pending_publication_;
            const std::string publication_id = pending.attempt_id.empty() ?
                observed[0][0].as<std::string>() : pending.attempt_id;
            const nlohmann::json& raw_capture = portfolio_config.at("config_inspection");
            bool valid = false;
            try {
                valid = raw_capture.dump().size() <= kInspectionMaxBytes &&
                        valid_pending_capture(raw_capture, strategy_allocations);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception&) {
                valid = false;
            }
            const bool version_valid = valid_ascii_token(pending.producer_version);
            const std::string captured_at = raw_capture.is_object() &&
                raw_capture.contains("captured_at") &&
                valid_utc_timestamp(raw_capture.at("captured_at")) ?
                raw_capture.at("captured_at").get<std::string>() : recorded_at;
            const std::string status = valid && version_valid ?
                raw_capture.at("status").get<std::string>() : "unavailable";
            const std::string reason = valid && version_valid ?
                raw_capture.at("reason").get<std::string>() : "capture_failed";
            const bool available = status == "available";
            nlohmann::json identity = {
                {"registry_id", pending.registry_id},
                {"registry_revision", pending.registry_revision},
                {"engine_strategy_id", pending.strategy_id},
                {"portfolio_id", pending.portfolio_id},
                {"run_date", pending.date},
                {"capture_id", publication_id},
                {"publication_id", publication_id},
                {"runtime_attempt_id", pending.attempt_id.empty() ? nlohmann::json(nullptr) :
                    nlohmann::json(pending.attempt_id)},
                {"producer_version", version_valid ? pending.producer_version : "unversioned"},
                {"control_mode", pending.attempt_id.empty() ? "uncontrolled" : "controlled"}};
            stored_portfolio_config["config_inspection"] = {
                {"publication_schema_version",
                 pending.evidence_requirement ==
                     PublicationEvidenceRequirement::RequiredFinalObservations ? 2 : 1},
                {"profile", "live_portfolio_runner_futures"},
                {"authority", "inspection_only"},
                {"stream", "system"},
                {"identity", std::move(identity)},
                {"captured_at", captured_at},
                {"publication_recorded_at", recorded_at},
                {"status", status}, {"reason", reason},
                {"supplied", available ? raw_capture.at("supplied") : nlohmann::json(nullptr)},
                {"selected_trend", available ? raw_capture.at("selected_trend") : nlohmann::json(nullptr)},
                {"consumption", {{"status", "not_collected"}}}};
            if (pending.evidence_requirement ==
                PublicationEvidenceRequirement::RequiredFinalObservations) {
                auto& sealed = stored_portfolio_config["config_inspection"];
                sealed["consumption"] = pending.final_consumption->document();
                if (!inspection_fits_transport(txn, sealed)) {
                    sealed["consumption"] = ConsumptionProjection::unavailable(
                        ConsumptionUnavailableReason::CapacityExceeded).document();
                    if (!inspection_fits_transport(txn, sealed)) {
                        sealed["status"] = "unavailable";
                        sealed["reason"] = "capture_failed";
                        sealed["supplied"] = nullptr;
                        sealed["selected_trend"] = nullptr;
                        if (!inspection_fits_transport(txn, sealed))
                            throw std::runtime_error("config_inspection_capacity_exceeded");
                    }
                }
            } else if (stored_portfolio_config.at("config_inspection").dump().size() > kInspectionMaxBytes) {
                auto& sealed = stored_portfolio_config["config_inspection"];
                sealed["status"] = "unavailable";
                sealed["reason"] = "capture_failed";
                sealed["supplied"] = nullptr;
                sealed["selected_trend"] = nullptr;
            }
        }


        // Format date as YYYY-MM-DD
        std::string date_str = format_timestamp(date).substr(0, 10);

        const std::string& actual_portfolio_id = portfolio_id;

        // Build INSERT with ON CONFLICT UPDATE
        // Unique constraint: (date, strategy_id, portfolio_id)
        std::string query = "INSERT INTO " + table_name +
                            " (date, strategy_id, portfolio_id, strategy_allocations, "
                            "portfolio_config, strategy_configs) "
                            "VALUES (" +
                            "'" + date_str + "', " + txn.quote(strategy_id) + ", " +
                            txn.quote(actual_portfolio_id) + ", " +
                            txn.quote(strategy_allocations.dump()) + "::jsonb, " +
                            txn.quote(stored_portfolio_config.dump()) + "::jsonb, " +
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
        if (has_inspection) {
            ERROR("Failed to store sealed live run metadata");
            return make_error<void>(ErrorCode::DATABASE_ERROR,
                                    "config_inspection_publication_write_failed");
        }
        ERROR("Failed to store live run metadata: " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::store_risk_limits(const std::string& strategy_id,
                                                 const std::string& portfolio_id,
                                                 const nlohmann::json& limits,
                                                 const std::string& table_name) {
    if (defer_live_write([this,strategy_id,portfolio_id,limits,table_name]() {
        return store_risk_limits(strategy_id,portfolio_id,limits,table_name);
    }, LimitsPart)) return Result<void>();
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate inputs before the connection so bad arguments are rejected the
    // same way with or without a live DB (and are unit-testable offline).
    auto table_validation = validate_table_name(table_name);
    if (table_validation.is_error()) {
        return table_validation;
    }

    auto strategy_validation = validate_strategy_id(strategy_id);
    if (strategy_validation.is_error()) {
        return strategy_validation;
    }

    auto validation = validate_connection();
    if (validation.is_error()) {
        return validation;
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id);


        // Append-only insert: risk_limits table is never updated or deleted.
        // A consumer gets the current envelope with: ORDER BY published_at DESC LIMIT 1.
        //
        // All values bound as parameters to prevent injection (commit f9e885f).
        std::string query = "INSERT INTO " + table_name +
                            " (strategy_id, portfolio_id, limits) "
                            "VALUES ($1, $2, $3::jsonb)";

        txn.exec(query, pqxx::params{strategy_id, portfolio_id, limits.dump()});
        txn.commit();

        INFO("Published risk limits for strategy=" + strategy_id + " portfolio=" + portfolio_id);

        return Result<void>();

    } catch (const std::exception& e) {
        // Failure to publish risk limits does NOT stop the trading run. AlgoLens treats
        // a missing envelope as "not evaluated" (yellow gate) rather than "pass" (green),
        // so a publish failure degrades safely. Log the error and continue.
        WARN("Failed to publish risk limits for strategy=" + strategy_id + " portfolio=" +
             portfolio_id + ": " + std::string(e.what()));
        return make_error<void>(ErrorCode::DATABASE_ERROR, e.what(), component_id_);
    }
}

Result<void> PostgresDatabase::update_equity_historical_metrics(
    const std::string& strategy_id,const std::string& portfolio_id,const Timestamp& date,
    const std::unordered_map<std::string,double>& updates,const std::vector<std::string>& null_columns) {
    static const std::set<std::string> columns {"volatility","sharpe_ratio","sortino_ratio",
        "max_drawdown","downside_deviation","win_rate","avg_win","avg_loss","profit_factor",
        "best_day","worst_day","gross_profit","gross_loss","winning_days","losing_days","total_days"};
    static const std::set<std::string> nullable {"sharpe_ratio","sortino_ratio","profit_factor"};
    if(portfolio_id.empty())return make_error<void>(ErrorCode::INVALID_ARGUMENT,"equity_book_required");
    auto admission=validate_operational_stream(portfolio_id,"system");if(admission.is_error())return admission;
    for(const auto& [column,value]:updates)
        if(!columns.count(column) || !std::isfinite(value))
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,"invalid_historical_metric");
    std::set<std::string> nulls;
    for(const auto& column:null_columns)
        if(!nullable.count(column) || updates.count(column) || !nulls.insert(column).second)
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,"invalid_null_historical_metric");
    if(defer_live_write(date,[this,strategy_id,portfolio_id,date,updates,null_columns]() {
        return update_equity_historical_metrics(strategy_id,portfolio_id,date,updates,null_columns);
    }))return Result<void>();
    std::lock_guard<std::mutex> lock(mutex_);
    auto connected=validate_connection();if(connected.is_error())return connected;
    auto owner=validate_strategy_id(strategy_id);if(owner.is_error())return owner;
    if(updates.empty() && nulls.empty())return Result<void>();
    try {
        PublicationTransaction txn(*connection_,publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,"system");
        std::string sql="UPDATE trading.live_results SET ";pqxx::params params;bool first=true;int index=1;
        for(const auto& [column,value]:updates) {
            if(!first)sql+=", ";first=false;
            sql+=column+"=$"+std::to_string(index++);params.append(value);
        }
        for(const auto& column:nulls) {if(!first)sql+=", ";first=false;sql+=column+"=NULL";}
        const int strategy_index=index++,book_index=index++,day_index=index++;
        sql+=" WHERE strategy_id=$"+std::to_string(strategy_index)+" AND portfolio_id=$"+
            std::to_string(book_index)+" AND date >= $"+std::to_string(day_index)+
            "::date AT TIME ZONE 'UTC' AND date < ($"+std::to_string(day_index)+
            "::date + INTERVAL '1 day') AT TIME ZONE 'UTC' AND portfolio_type='system'";
        params.append(strategy_id);params.append(portfolio_id);params.append(format_timestamp(date).substr(0,10));
        txn.exec(sql,params);txn.commit();return Result<void>();
    } catch(const std::exception& error) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,error.what(),component_id_);
    }
}

}  // namespace trade_ngin
