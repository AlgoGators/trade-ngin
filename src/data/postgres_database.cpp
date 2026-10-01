// src/data/postgres_database.cpp

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/qt_empty_model_owner_storage.hpp"
#include "trade_ngin/data/market_data_utils.hpp"
#include <iomanip>
#include <limits>
#include <sstream>
#include <cctype>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/qt_sha256.hpp"
#include "trade_ngin/portfolio/qt_wire.hpp"
#include "trade_ngin/core/time_utils.hpp"

#include "trade_ngin/data/market_data_bus.hpp"

namespace {
// Builds "($1,...,$cols),($cols+1,...,$2*cols),..." for `rows` row-groups of `cols`
// placeholders each, so a multi-row INSERT can bind every value as a parameter instead
// of concatenating it into the query text. Chunk callers to stay under Postgres's
// 65535-parameter-per-query limit (rows * cols <= 65535).
std::string build_value_placeholders(size_t rows, size_t cols) {
    std::string result;
    result.reserve(rows * cols * 4);
    size_t param_idx = 1;
    for (size_t r = 0; r < rows; ++r) {
        if (r > 0)
            result += ",";
        result += "(";
        for (size_t c = 0; c < cols; ++c) {
            if (c > 0)
                result += ",";
            result += "$" + std::to_string(param_idx++);
        }
        result += ")";
    }
    return result;
}

bool valid_proposal_day(const std::string& day) {
    if (day.size()!=10 || day[4]!='-' || day[7]!='-') return false;
    for (size_t i=0;i<day.size();++i)
        if (i!=4 && i!=7 && !std::isdigit(static_cast<unsigned char>(day[i]))) return false;
    const std::chrono::year_month_day parsed{
        std::chrono::year{std::stoi(day.substr(0,4))},
        std::chrono::month{static_cast<unsigned>(std::stoi(day.substr(5,2)))},
        std::chrono::day{static_cast<unsigned>(std::stoi(day.substr(8,2)))}};
    return parsed.ok();
}

bool nonblank_proposal_identity(const std::string& value) {
    return value.find_first_not_of(" \t\r\n") != std::string::npos;
}

// PostgreSQL returns numeric columns as fixed-point decimal text. Decode only
// values exactly representable by Decimal's signed, eight-place raw int64.
trade_ngin::Decimal exact_position_decimal(const std::string& text) {
    constexpr uint64_t scale = 100000000ULL;
    size_t index = 0;
    bool negative = false;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        ++index;
    }
    const uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) +
                           static_cast<uint64_t>(negative);
    const uint64_t max_whole = limit / scale;
    uint64_t whole = 0;
    uint64_t fraction = 0;
    size_t fraction_digits = 0;
    bool has_digit = false;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        const uint64_t digit = static_cast<uint64_t>(text[index++] - '0');
        has_digit = true;
        if (whole > max_whole / 10 ||
            (whole == max_whole / 10 && digit > max_whole % 10))
            throw std::out_of_range("strict_position_numeric_out_of_range");
        whole = whole * 10 + digit;
    }
    if (index < text.size() && text[index] == '.') {
        ++index;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            const uint64_t digit = static_cast<uint64_t>(text[index++] - '0');
            has_digit = true;
            if (fraction_digits < 8) {
                fraction = fraction * 10 + digit;
                ++fraction_digits;
            } else if (digit != 0) {
                throw std::invalid_argument("strict_position_numeric_excess_precision");
            }
        }
    }
    if (!has_digit || index != text.size())
        throw std::invalid_argument("strict_position_numeric_invalid");
    while (fraction_digits < 8) {
        fraction *= 10;
        ++fraction_digits;
    }
    const uint64_t magnitude = whole * scale + fraction;
    if (magnitude > limit)
        throw std::out_of_range("strict_position_numeric_out_of_range");
    const int64_t raw = negative
        ? (magnitude == limit ? std::numeric_limits<int64_t>::min()
                              : -static_cast<int64_t>(magnitude))
        : static_cast<int64_t>(magnitude);
    return trade_ngin::Decimal::from_raw(raw);
}
}  // namespace

namespace trade_ngin {

PostgresDatabase::PostgresDatabase(std::string connection_string)
    : connection_string_(std::move(connection_string)), connection_(nullptr) {
    Logger::register_component("PostgresDatabase");
}

PostgresDatabase::~PostgresDatabase() {
    try {
        abandon_live_publication();
        disconnect();
    } catch (const std::exception& e) {
        WARN("Exception in PostgresDatabase destructor: " + std::string(e.what()));
    } catch (...) {
        WARN("Unknown exception in PostgresDatabase destructor");
    }
}

Result<void> PostgresDatabase::connect() {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        connection_ = std::make_unique<pqxx::connection>(connection_string_);
        if (!connection_->is_open()) {
            return make_error<void>(ErrorCode::CONNECTION_ERROR,
                                    "Failed to open database connection", "PostgresDatabase");
        }

        // Generate a unique ID for this connection instance
        static std::atomic<int> counter{0};
        std::string unique_id = "POSTGRES_DB_" + std::to_string(++counter);

        // Register with state manager using the unique ID
        ComponentInfo info{ComponentType::MARKET_DATA,
                           ComponentState::INITIALIZED,
                           unique_id,
                           "",
                           std::chrono::system_clock::now(),
                           {}};

        auto register_result = StateManager::instance().register_component(info);
        if (register_result.is_error()) {
            // Log the error but don't fail the connection - it's still usable
            WARN("Failed to register database with StateManager: " +
                 std::string(register_result.error()->what()));
        } else {
            // Store the generated ID for later use in disconnect()
            component_id_ = unique_id;
            (void)StateManager::instance().update_state(component_id_, ComponentState::RUNNING);
            INFO("Successfully connected to PostgreSQL database with ID: " + component_id_);
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR,
                                "Database connection error: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

void PostgresDatabase::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_ && connection_->is_open()) {
        connection_->close();
        connection_.reset();

        // Only attempt to unregister if we have a valid component ID
        if (!component_id_.empty()) {
            try {
                (void)StateManager::instance().unregister_component(component_id_);
            } catch (const std::exception& e) {
                WARN("Error unregistering database component: " + std::string(e.what()));
            }
            component_id_.clear();
        }

        INFO("Disconnected from PostgreSQL database");
    }
}

bool PostgresDatabase::is_connected() const {
    return connection_ && connection_->is_open();
}

Result<PostgresDatabase::MarketDataSnapshot> PostgresDatabase::read_market_data_snapshot(
    pqxx::work& transaction, const std::vector<std::string>& symbols,
    const Timestamp& start_date, const Timestamp& end_date, AssetClass asset_class,
    DataFrequency frequency, const std::string& data_type) {
    if (start_date > end_date)
        return make_error<MarketDataSnapshot>(ErrorCode::INVALID_ARGUMENT,
            "Start date must be before end date");
    auto rows = execute_market_data_query(symbols, start_date, end_date,
        asset_class, frequency, data_type, transaction);
    if (rows.is_error()) return make_error<MarketDataSnapshot>(
        rows.error()->code(), rows.error()->what());
    // The immutable actual-source reader requires at least one actual bar.
    // Refuse before the legacy converter's null-array empty-table branch.
    if (rows.value().empty()) return make_error<MarketDataSnapshot>(
        ErrorCode::INVALID_DATA, "No market bars available for source capture");
    auto table = convert_to_arrow_table(rows.value());
    if (table.is_error()) return make_error<MarketDataSnapshot>(
        table.error()->code(), table.error()->what());
    return MarketDataSnapshot{rows.value(), table.value()};
}
Result<std::shared_ptr<arrow::Table>> PostgresDatabase::get_market_data(
    const std::vector<std::string>& symbols, const Timestamp& start_date, const Timestamp& end_date,
    AssetClass asset_class, DataFrequency freq, const std::string& data_type) {
    if (start_date > end_date) {
        return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::INVALID_ARGUMENT,
                                                         "Start date must be before end date");
    }

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::shared_ptr<arrow::Table>>(validation.error()->code(),
                                                         validation.error()->what());
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        auto query_result = execute_market_data_query(symbols, start_date, end_date, asset_class,
                                                      freq, data_type, txn);

        if (query_result.is_error()) {
            return make_error<std::shared_ptr<arrow::Table>>(query_result.error()->code(),
                                                             query_result.error()->what());
        }

        auto result = query_result.value();
        txn.commit();

        // Convert to Arrow table
        auto table_result = convert_to_arrow_table(result);
        if (table_result.is_error()) {
            return table_result;
        }

        // Publish market data events
        for (const auto& row : result) {
            MarketDataEvent event;
            event.type = MarketDataEventType::BAR;
            event.symbol = row["symbol"].as<std::string>();

            // Parse timestamp
            std::string time_str = row["time"].as<std::string>();
            std::tm time_info = {};
            std::istringstream ss(time_str);
            ss >> std::get_time(&time_info, "%Y-%m-%d %H:%M:%S");
            time_t time_val = std::mktime(&time_info);
            trade_ngin::core::safe_gmtime(&time_val, &time_info);
            event.timestamp = std::chrono::system_clock::from_time_t(std::mktime(&time_info));

            // Add numeric fields
            event.numeric_fields["open"] = row["open"].as<double>();
            event.numeric_fields["high"] = row["high"].as<double>();
            event.numeric_fields["low"] = row["low"].as<double>();
            event.numeric_fields["close"] = row["close"].as<double>();
            event.numeric_fields["volume"] = row["volume"].as<double>();

            MarketDataBus::instance().publish(event);
        }

        return table_result;

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::DATABASE_ERROR, "Failed to fetch market data: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_executions(const std::vector<ExecutionReport>& executions,
                                                const std::string& strategy_id,
                                                const std::string& strategy_name,
                                                const std::string& portfolio_id,
                                                const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (!executions.empty() && defer_live_write(executions.front().fill_time,
        [this,executions,strategy_id,strategy_name,portfolio_id,table_name,portfolio_type]() {
            return store_executions(executions,strategy_id,strategy_name,portfolio_id,table_name,portfolio_type);
        })) return Result<void>();
    std::cout << "DEBUG: store_executions called with " << executions.size() << " executions"
              << " for strategy_id: " << strategy_id << " strategy_name: " << strategy_name
              << " portfolio_id: " << portfolio_id << std::endl;

    auto validation = validate_connection();
    if (validation.is_error()) {
        std::cout << "DEBUG: Connection validation failed" << std::endl;
        return validation;
    }
    std::cout << "DEBUG: Connection validation passed" << std::endl;

    try {
        // Validate table name
        std::cout << "DEBUG: Validating table name: " << table_name << std::endl;
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            std::cout << "DEBUG: Table validation failed: " << table_validation.error()->what()
                      << std::endl;
            return table_validation;
        }
        std::cout << "DEBUG: Table validation passed" << std::endl;

        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);
        // Cleanup and inserts share both the full scope and transaction.
        for (const auto& execution : executions) {
            txn.exec("DELETE FROM " + table_name + " WHERE strategy_id=$1 AND strategy_name=$2 "
                     "AND portfolio_id=$3 AND date=$4::date AND order_id=$5 AND portfolio_type=$6",
                     pqxx::params{strategy_id,strategy_name,portfolio_id,
                                  format_timestamp(execution.fill_time).substr(0,10),execution.order_id,portfolio_type});
        }

        for (const auto& exec : executions) {
            std::cout << "DEBUG: Processing execution for symbol: " << exec.symbol << std::endl;

            // Validate execution data
            std::cout << "DEBUG: About to validate execution report" << std::endl;
            auto exec_validation = validate_execution_report(exec);
            if (exec_validation.is_error()) {
                std::cout << "DEBUG: Execution validation failed: "
                          << exec_validation.error()->what() << std::endl;
                return exec_validation;
            }
            std::cout << "DEBUG: Execution validation passed" << std::endl;

            // Extract date from fill_time for date column
            auto fill_time_t = std::chrono::system_clock::to_time_t(exec.fill_time);
            std::stringstream date_ss;
            date_ss << std::put_time(std::gmtime(&fill_time_t), "%Y-%m-%d");
            std::string exec_date = date_ss.str();

            // Keep the sleeve's gross/as-if cost and its account-netting
            // adjustment separately. The derived net cost is their difference.
            std::string query = "INSERT INTO " + table_name +
                                " (exec_id, order_id, symbol, side, quantity, price, "
                                "execution_time, commissions_fees, implicit_price_impact, "
                                "slippage_market_impact, total_transaction_costs, netting_adjustment, is_partial, "
                                "strategy_id, strategy_name, date, portfolio_id, portfolio_type) VALUES "
                                "($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18)";

            std::cout << "DEBUG: About to execute SQL query" << std::endl;
            std::cout << "DEBUG: Query: " << query << std::endl;

            // Updated exec to include all 4 cost fields
            txn.exec(
                query, pqxx::params{
                exec.exec_id, exec.order_id, exec.symbol, side_to_string(exec.side),
                static_cast<double>(exec.filled_quantity), static_cast<double>(exec.fill_price),
                format_timestamp(exec.fill_time),
                static_cast<double>(exec.commissions_fees),         // $8
                static_cast<double>(exec.implicit_price_impact),    // $9
                static_cast<double>(exec.slippage_market_impact),   // $10
                static_cast<double>(exec.total_transaction_costs),  // $11
                static_cast<double>(exec.netting_adjustment),       // $12
                exec.is_partial,                                    // $13
                strategy_id,    // $14 - combined (e.g., LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST)
                strategy_name,  // $15 - individual (e.g., TREND_FOLLOWING)
                exec_date,      // $16
                portfolio_id,portfolio_type});

            std::cout << "DEBUG: SQL executed successfully for " << exec.symbol << std::endl;
        }

        std::cout << "DEBUG: About to commit transaction" << std::endl;
        txn.commit();
        std::cout << "DEBUG: Transaction committed successfully" << std::endl;

        return Result<void>();
    } catch (const std::exception& e) {
        std::cout << "DEBUG: Exception caught: " << e.what() << std::endl;
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store executions: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

std::string PostgresDatabase::format_timestamp(const Timestamp& ts) {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::stringstream ss;
    std::tm time_info;
    trade_ngin::core::safe_gmtime(&time_t, &time_info);
    ss << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

Result<void> PostgresDatabase::validate_connection() const {
    if (!is_connected() || !connection_ || !connection_->is_open()) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR, "Not connected to database",
                                "PostgresDatabase");
    }
    return Result<void>();
}

bool PostgresDatabase::column_exists(pqxx::work& txn, const std::string& qualified_table,
                                     const std::string& column) const {
    auto dot = qualified_table.find('.');
    std::string schema = (dot == std::string::npos) ? "public" : qualified_table.substr(0, dot);
    std::string table = (dot == std::string::npos) ? qualified_table
                                                   : qualified_table.substr(dot + 1);
    try {
        auto result = txn.exec("SELECT 1 FROM information_schema.columns "
                               "WHERE table_schema = $1 AND table_name = $2 AND column_name = $3",
                               pqxx::params{schema, table, column});
        return !result.empty();
    } catch (const std::exception& e) {
        WARN("Could not determine whether " + qualified_table + "." + column +
             " exists, assuming it does not: " + std::string(e.what()));
        return false;
    }
}

Result<void> PostgresDatabase::store_positions(const std::vector<Position>& positions,
                                               const std::string& strategy_id,
                                               const std::string& strategy_name,
                                               const std::string& portfolio_id,
                                               const std::string& table_name,
                                               const std::string& portfolio_type) {
    if (portfolio_type == "qt_proposal") {
        poison_proposal_refusal();
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"proposal_positions_require_specialized_api");
    }
    auto stream_validation = validate_operational_stream(portfolio_id, portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (pending_publication_ && !publication_transaction_ && !positions.empty()) {
        const auto day = format_timestamp(positions.front().last_update).substr(0,10);
        bool valid_batch = true;
        for (const auto& position : positions)
            valid_batch = valid_batch && format_timestamp(position.last_update).substr(0,10) == day;
        if (day == pending_publication_->date && portfolio_type == "system") {
            const auto& configured = pending_publication_->snapshot.at("strategies");
            valid_batch = valid_batch && strategy_id == pending_publication_->strategy_id &&
                portfolio_id == pending_publication_->portfolio_id && table_name == "trading.positions" &&
                configured.contains(strategy_name) && configured.at(strategy_name).value("enabled_live",false);
            if (valid_batch && pending_publication_->proposal_sealed_members.contains(strategy_name)) {
                pending_publication_->invalid_payload = true;
                return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_proposal_source_sealed");
            }
            if (valid_batch && std::find(pending_publication_->fresh_system_members.begin(),
                pending_publication_->fresh_system_members.end(),strategy_name) ==
                pending_publication_->fresh_system_members.end())
                pending_publication_->fresh_system_members.push_back(strategy_name);
        }
        if (!valid_batch) {
            pending_publication_->invalid_payload = true;
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_position_batch_invalid");
        }
        if (day == pending_publication_->date && portfolio_type == "system") {
            auto& captured = pending_publication_->fresh_system_components;
            std::erase_if(captured, [&](const QtSeedRow& row) {
                return row.key.strategy_name == strategy_name;
            });
            for (const auto& position : positions)
                captured.push_back({{portfolio_id,strategy_id,strategy_name,day,
                                     position.symbol,"system"},
                                    position.quantity,position.average_price});
        }
    }
    if (!positions.empty() && defer_live_write(positions.front().last_update,
        [this,positions,strategy_id,strategy_name,portfolio_id,table_name,portfolio_type]() {
            return store_positions(positions,strategy_id,strategy_name,portfolio_id,table_name,portfolio_type);
        }, portfolio_type == "system" ? static_cast<unsigned>(PositionsPart) : 0u)) return Result<void>();
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        // Begin transaction
        // The transaction owner establishes BEGIN; borrowed publication methods do not.

        // Has the dual-portfolio migration been applied? Detecting this at runtime means this
        // code is correct against both an upgraded and a not-yet-upgraded database, so rolling
        // out the binary and running the migration can happen in either order.
        const bool has_portfolio_type = column_exists(txn, table_name, "portfolio_type");
        if (!has_portfolio_type && portfolio_type != "system") {
            // Refuse rather than silently writing the qt stream into the system stream --
            // that would corrupt the very comparison this feature exists to enable.
            return make_error<void>(
                ErrorCode::DATABASE_ERROR,
                "portfolio_type='" + portfolio_type + "' requested but " + table_name +
                    " has no portfolio_type column. Apply migrations/001_add_portfolio_type.sql "
                    "before writing a non-system stream.",
                "PostgresDatabase");
        }

        // Clear existing positions for this strategy (by strategy_id AND strategy_name)
        // and the date of the positions being inserted.
        // CRITICAL: Must filter by BOTH strategy_id and strategy_name, otherwise positions from
        // other strategies with the same combined strategy_id will be deleted!
        try {
            // Get the date from the first position being inserted (all positions should be from the
            // same date)
            if (!positions.empty()) {
                auto time_t = std::chrono::system_clock::to_time_t(positions[0].last_update);
                std::stringstream ss;
                ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
                std::string position_date = ss.str();

                // EQUALLY CRITICAL: scope the delete to this stream. Without the portfolio_type
                // predicate, rewriting the system stream would delete that day's qt positions --
                // silently destroying QT's decisions every time the engine ran.
                std::string delete_query = "DELETE FROM " + table_name +
                                           " WHERE strategy_id = $1 AND strategy_name = $2"
                                           " AND portfolio_id = $3 AND DATE(last_update) = $4";
                if (has_portfolio_type) {
                    delete_query += " AND portfolio_type = $5";
                }
                DEBUG("Deleting existing positions for strategy_id=" + strategy_id +
                      " strategy_name=" + strategy_name + " portfolio_id=" + portfolio_id +
                      " date=" + position_date + " portfolio_type=" + portfolio_type);
                if (has_portfolio_type) {
                    txn.exec(delete_query, pqxx::params{strategy_id, strategy_name, portfolio_id,
                                                        position_date, portfolio_type});
                } else {
                    txn.exec(delete_query,
                             pqxx::params{strategy_id, strategy_name, portfolio_id, position_date});
                }
            }
        } catch (const std::exception& e) {
            // If strategy_id/strategy_name columns don't exist, clear all positions for the
            // position date only
            WARN(
                "strategy_id/strategy_name columns may not exist, clearing all positions for "
                "position date: " +
                std::string(e.what()));

            if (!positions.empty()) {
                auto time_t = std::chrono::system_clock::to_time_t(positions[0].last_update);
                std::stringstream ss;
                ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
                std::string position_date = ss.str();

                std::string delete_query =
                    "DELETE FROM " + table_name + " WHERE DATE(last_update) = $1";
                txn.exec(delete_query, pqxx::params{position_date});
            }
        }

        // Validate every position up front; build (position_date, position) pairs for insertion.
        // Schema: symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
        //         last_update, updated_at, strategy_id, strategy_name, date, portfolio_id
        std::vector<std::pair<std::string, const Position*>> rows;
        rows.reserve(positions.size());
        for (const auto& pos : positions) {
            DEBUG("Position before validation: " + pos.symbol +
                  " qty=" + std::to_string(static_cast<double>(pos.quantity)) +
                  " avg_price=" + std::to_string(static_cast<double>(pos.average_price)) +
                  " unrealized=" + std::to_string(static_cast<double>(pos.unrealized_pnl)) +
                  " realized=" + std::to_string(static_cast<double>(pos.realized_pnl)));

            auto pos_validation = validate_position(pos);
            if (pos_validation.is_error()) {
                ERROR("Position validation failed for " + pos.symbol + ": " +
                      pos_validation.error()->what());
                return pos_validation;
            }

            auto time_t = std::chrono::system_clock::to_time_t(pos.last_update);
            std::stringstream date_ss;
            date_ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
            rows.emplace_back(date_ss.str(), &pos);
        }

        if (!rows.empty()) {
            // Try with strategy_id/strategy_name columns first; every value bound as a parameter.
            try {
                std::string query =
                    "INSERT INTO " + table_name +
                    " (symbol, quantity, average_price, daily_unrealized_pnl, "
                    "daily_realized_pnl, last_update, updated_at, strategy_id, "
                    "strategy_name, date, portfolio_id" +
                    std::string(has_portfolio_type ? ", portfolio_type" : "") +
                    ") VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11" +
                    std::string(has_portfolio_type ? ", $12" : "") + ")";
                for (const auto& [position_date, pos] : rows) {
                    pqxx::params p{pos->symbol,
                                   static_cast<double>(pos->quantity),
                                   static_cast<double>(pos->average_price),
                                   static_cast<double>(pos->unrealized_pnl),
                                   static_cast<double>(pos->realized_pnl),
                                   format_timestamp(pos->last_update),
                                   format_timestamp(pos->last_update),
                                   strategy_id,
                                   strategy_name,
                                   position_date,
                                   portfolio_id};
                    if (has_portfolio_type) {
                        p.append(portfolio_type);
                    }
                    txn.exec(query, p);
                }
            } catch (const std::exception& e) {
                // If strategy_id column doesn't exist, retry the whole batch without it.
                WARN("strategy_id column may not exist, trying without it: " +
                     std::string(e.what()));

                std::string query =
                    "INSERT INTO " + table_name +
                    " (symbol, quantity, average_price, daily_unrealized_pnl, "
                    "daily_realized_pnl, last_update, updated_at, strategy_id, "
                    "strategy_name, date, portfolio_id) VALUES "
                    "($1, $2, $3, $4, $5, $6, $7, '', '', $8, 'BASE_PORTFOLIO')";
                for (const auto& [position_date, pos] : rows) {
                    txn.exec(query, pqxx::params{pos->symbol, static_cast<double>(pos->quantity),
                                                 static_cast<double>(pos->average_price),
                                                 static_cast<double>(pos->unrealized_pnl),
                                                 static_cast<double>(pos->realized_pnl),
                                                 format_timestamp(pos->last_update),
                                                 format_timestamp(pos->last_update),
                                                 position_date});
                }
            }
        }

        txn.commit();
        INFO("Successfully updated " + std::to_string(positions.size()) + " positions");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store positions: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_signals(const std::unordered_map<std::string, double>& signals,
                                             const std::string& strategy_id,
                                             const std::string& strategy_name,
                                             const std::string& portfolio_id,
                                             const Timestamp& timestamp,
                                             const std::string& table_name) {
    auto stream_validation = validate_operational_stream(portfolio_id, "system");
    if (stream_validation.is_error()) return stream_validation;
    if (defer_live_write(timestamp,[this,signals,strategy_id,strategy_name,portfolio_id,timestamp,table_name]() {
        return store_signals(signals,strategy_id,strategy_name,portfolio_id,timestamp,table_name);
    })) return Result<void>();
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id);


        // Validate table name and strategy ID
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        auto strategy_validation = validate_strategy_id(strategy_id);
        if (strategy_validation.is_error()) {
            return strategy_validation;
        }

        for (const auto& [symbol, signal] : signals) {
            // Validate signal data
            auto signal_validation = validate_signal_data(symbol, signal);
            if (signal_validation.is_error()) {
                return signal_validation;
            }

            std::string query =
                "INSERT INTO " + table_name +
                " (strategy_id, symbol, signal_value, timestamp, portfolio_id, strategy_name) "
                "VALUES "
                "($1, $2, $3, $4, $5, $6) "
                "ON CONFLICT (portfolio_id, strategy_id, strategy_name, symbol, timestamp) "
                "DO UPDATE SET signal_value = EXCLUDED.signal_value";

            txn.exec(query, pqxx::params{strategy_id, symbol, signal, format_timestamp(timestamp),
                            portfolio_id, strategy_name});
        }

        txn.commit();
        INFO("Successfully stored signals for strategy: " + strategy_id);
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store signals: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<std::vector<std::string>> PostgresDatabase::get_symbols(AssetClass asset_class,
                                                               DataFrequency freq,
                                                               const std::string& data_type) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<std::string>>(validation.error()->code(),
                                                    validation.error()->what());
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<std::vector<std::string>>(table_validation.error()->code(),
                                                        table_validation.error()->what());
        }

        std::string query =
            "WITH latest_data AS ("
            "   SELECT DISTINCT ON (symbol) symbol, time "
            "   FROM " +
            full_table_name +
            " "
            "   ORDER BY symbol, time DESC"
            ") "
            "SELECT symbol "
            "FROM latest_data "
            "ORDER BY symbol";

        auto result = txn.exec(query);

        std::vector<std::string> symbols;
        symbols.reserve(result.size());

        for (const auto& row : result) {
            symbols.push_back(row[0].as<std::string>());
        }

        txn.commit();
        DEBUG("Retrieved " + std::to_string(symbols.size()) + " symbols from " + full_table_name);
        return Result<std::vector<std::string>>(symbols);

    } catch (const std::exception& e) {
        return make_error<std::vector<std::string>>(
            ErrorCode::DATABASE_ERROR, "Failed to get symbols: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, double>> PostgresDatabase::get_latest_prices(
    const std::vector<std::string>& symbols, AssetClass asset_class, DataFrequency freq,
    const std::string& data_type) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(validation.error()->code(),
                                                                   validation.error()->what());
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<std::unordered_map<std::string, double>>(
                table_validation.error()->code(), table_validation.error()->what());
        }

        // Validate symbols
        auto symbol_validation = validate_symbols(symbols);
        if (symbol_validation.is_error()) {
            return make_error<std::unordered_map<std::string, double>>(
                symbol_validation.error()->code(), symbol_validation.error()->what());
        }

        // Query to get latest close price for each symbol
        std::string query =
            "SELECT DISTINCT ON (symbol) symbol, close "
            "FROM " +
            full_table_name +
            " "
            "WHERE symbol = ANY($1) "
            "ORDER BY symbol, time DESC";

        auto result = txn.exec(query, pqxx::params{symbols});
        txn.commit();

        std::unordered_map<std::string, double> prices;
        for (const auto& row : result) {
            std::string symbol = row[0].as<std::string>();
            double price = row[1].as<double>();
            prices[symbol] = price;
        }

        DEBUG("Retrieved latest prices for " + std::to_string(prices.size()) + " symbols from " +
              full_table_name);
        return Result<std::unordered_map<std::string, double>>(prices);

    } catch (const std::exception& e) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::DATABASE_ERROR, "Failed to get latest prices: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, Position>> PostgresDatabase::load_positions_by_date(
    const std::string& strategy_id, const std::string& strategy_name,
    const std::string& portfolio_id, const Timestamp& date, const std::string& table_name,
    const std::string& portfolio_type) {
    if (portfolio_type == "qt_proposal") {
        poison_proposal_refusal();
        return make_error<std::unordered_map<std::string,Position>>(
            ErrorCode::INVALID_ARGUMENT,"proposal_positions_require_specialized_reader");
    }
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) {
        return make_error<std::unordered_map<std::string, Position>>(
            portfolio_validation.error()->code(), portfolio_validation.error()->what());
    }
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::unordered_map<std::string, Position>>(validation.error()->code(),
                                                                     validation.error()->what());
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Detected at runtime so this works against both an upgraded and a
        // not-yet-upgraded database -- see store_positions for the rationale.
        const bool has_portfolio_type = column_exists(txn, table_name, "portfolio_type");
        const std::string stream_filter =
            has_portfolio_type ? " AND portfolio_type = $STREAM" : "";

        std::string date_str = format_timestamp(date);
        pqxx::result result;

        const std::string& actual_portfolio_id = portfolio_id;

        // Query to get positions for a specific strategy, portfolio, and date
        // If strategy_name is provided, filter by strategy_id, strategy_name, AND portfolio_id
        // This ensures we only get positions from the specific portfolio and strategy
        if (!strategy_name.empty()) {
            std::string query =
                "SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, "
                "last_update "
                "FROM " +
                table_name +
                " "
                "WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3 AND "
                "DATE(last_update) = DATE($4)";
            {
                std::string f = stream_filter;
                auto pos = f.find("$STREAM");
                if (pos != std::string::npos) f.replace(pos, 7, "$5");
                query += f;
            }

            DEBUG("Querying positions for strategy_id: " + strategy_id + ", strategy_name: " +
                  strategy_name + ", portfolio_id: " + actual_portfolio_id + ", date: " + date_str);
            DEBUG("Full query: " + query);
            {
                pqxx::params p{strategy_id, strategy_name, actual_portfolio_id, date_str};
                if (has_portfolio_type) p.append(portfolio_type);
                result = txn.exec(query, p);
            }
        } else {
            // If strategy_name is empty, filter by strategy_id and portfolio_id (for aggregate
            // loading)
            std::string query =
                "SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, "
                "last_update "
                "FROM " +
                table_name +
                " "
                "WHERE strategy_id = $1 AND portfolio_id = $2 AND DATE(last_update) = DATE($3)";
            {
                std::string f = stream_filter;
                auto pos = f.find("$STREAM");
                if (pos != std::string::npos) f.replace(pos, 7, "$4");
                query += f;
            }

            DEBUG("Querying positions for strategy_id: " + strategy_id +
                  ", portfolio_id: " + actual_portfolio_id + ", date: " + date_str +
                  ", portfolio_type: " + portfolio_type);
            DEBUG("Full query: " + query);
            {
                pqxx::params p{strategy_id, actual_portfolio_id, date_str};
                if (has_portfolio_type) p.append(portfolio_type);
                result = txn.exec(query, p);
            }
        }
        txn.commit();

        DEBUG("Query returned " + std::to_string(result.size()) + " rows");
        std::unordered_map<std::string, Position> positions;
        for (const auto& row : result) {
            std::string symbol = row[0].as<std::string>();
            double quantity = row[1].as<double>();
            double avg_price = row[2].as<double>();
            double unrealized_pnl = row[3].as<double>();
            double realized_pnl = row[4].as<double>();

            // Parse timestamp directly from database - pqxx handles the conversion
            Timestamp last_update;
            try {
                // Try to parse as timestamp
                std::string last_update_str = row[5].as<std::string>();
                std::tm tm = {};
                std::istringstream ss(last_update_str);
                ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                if (!ss.fail()) {
                    auto time_c = std::mktime(&tm);
                    last_update = std::chrono::system_clock::from_time_t(time_c);
                } else {
                    // Fall back to current time if parsing fails
                    WARN("Failed to parse timestamp: " + last_update_str + ", using current time");
                    last_update = std::chrono::system_clock::now();
                }
            } catch (const std::exception& e) {
                WARN("Exception parsing timestamp: " + std::string(e.what()) +
                     ", using current time");
                last_update = std::chrono::system_clock::now();
            }

            Position pos;
            pos.symbol = symbol;
            pos.quantity = Decimal(quantity);
            pos.average_price = Decimal(avg_price);
            pos.unrealized_pnl = Decimal(unrealized_pnl);
            pos.realized_pnl = Decimal(realized_pnl);
            pos.last_update = last_update;

            positions[symbol] = pos;
        }

        DEBUG("Loaded " + std::to_string(positions.size()) + " positions for strategy " +
              strategy_id + " on " + date_str);
        return Result<std::unordered_map<std::string, Position>>(positions);

    } catch (const std::exception& e) {
        return make_error<std::unordered_map<std::string, Position>>(
            ErrorCode::DATABASE_ERROR, "Failed to load positions by date: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<PostgresDatabase::ReportPositionRows> PostgresDatabase::load_report_positions_by_date(
    const std::string& strategy_id, const std::vector<std::string>& strategy_names,
    const std::string& portfolio_id, const Timestamp& report_date,
    const std::string& portfolio_type) {
    if (portfolio_type == "qt_proposal") {
        poison_proposal_refusal();
        return make_error<ReportPositionRows>(ErrorCode::INVALID_ARGUMENT,
            "proposal_positions_require_specialized_reader");
    }
    if (portfolio_type != "system" && portfolio_type != "qt" &&
        portfolio_type != "benchmark" && portfolio_type != "benchmark_rebench" &&
        portfolio_type != "benchmark_frozen_shadow")
        return make_error<ReportPositionRows>(ErrorCode::INVALID_ARGUMENT,
            "report_position_stream_unsupported");
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<ReportPositionRows>(
            validation.error()->code(),
            "Failed to load strict report positions for portfolio " + portfolio_id +
                ", strategy " + strategy_id + ": " + validation.error()->what(),
            "PostgresDatabase");
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        if (!column_exists(txn, "trading.positions", "portfolio_type")) {
            return make_error<ReportPositionRows>(
                ErrorCode::DATABASE_ERROR,
                "trading.positions.portfolio_type is required for strict report position reads "
                "for portfolio " + portfolio_id + ", strategy " + strategy_id,
                "PostgresDatabase");
        }

        const std::string date_str = format_timestamp(report_date);
        std::string quoted_names;
        for (const auto& name : strategy_names) {
            if (!quoted_names.empty()) quoted_names += ",";
            quoted_names += txn.quote(name);
        }
        if (quoted_names.empty()) quoted_names = "NULL";
        const std::string query =
            "SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, "
            "last_update, strategy_name FROM trading.positions "
            "WHERE strategy_id = $1 AND portfolio_id = $2 AND "
            "date = $3 AND portfolio_type = $4 AND strategy_name IN (" + quoted_names + ")";
        const auto result = txn.exec(
            query, pqxx::params{strategy_id, portfolio_id, date_str, portfolio_type});
        txn.commit();

        ReportPositionRows positions;
        for (const auto& row : result) {
            Timestamp last_update;
            try {
                const std::string last_update_str = row[5].as<std::string>();
                std::tm tm = {};
                std::istringstream stream(last_update_str);
                stream >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                if (stream.fail()) {
                    WARN("Failed to parse timestamp: " + last_update_str + ", using current time");
                    last_update = std::chrono::system_clock::now();
                } else {
                    last_update = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                }
            } catch (const std::exception& e) {
                WARN("Exception parsing timestamp: " + std::string(e.what()) +
                     ", using current time");
                last_update = std::chrono::system_clock::now();
            }

            const std::string symbol = row[0].as<std::string>();
            positions[row[6].as<std::string>()][symbol] = Position(symbol,
                exact_position_decimal(row[1].as<std::string>()),
                exact_position_decimal(row[2].as<std::string>()),
                exact_position_decimal(row[3].as<std::string>()),
                exact_position_decimal(row[4].as<std::string>()), last_update);
        }

        return Result<ReportPositionRows>(std::move(positions));
    } catch (const std::exception& e) {
        return make_error<ReportPositionRows>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load strict report positions for portfolio " + portfolio_id +
                ", strategy " + strategy_id + ": " + e.what(),
            "PostgresDatabase");
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::execute_query(const std::string& query) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::shared_ptr<arrow::Table>>(validation.error()->code(),
                                                         validation.error()->what());
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        // During compute, generic queries are reads only. Typed current-day
        // writes join the payload; the previous-day finalizer is explicitly scoped.
        if (pending_publication_ && !publication_transaction_)
            txn.exec("SET TRANSACTION READ ONLY");
        auto result = txn.exec(query);
        txn.commit();

        // Use generic converter that doesn't assume specific columns
        return convert_generic_to_arrow(result);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::DATABASE_ERROR, "Failed to execute query: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::execute_direct_query(const std::string& query) {
    if (pending_publication_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_unscoped_write_refused");
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<void>(validation.error()->code(), validation.error()->what());
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        txn.exec(query);
        txn.commit();
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to execute direct query: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::get_contract_metadata() const {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            validation.error()->code(), validation.error()->what(), "PostgresDatabase");
    }

    try {
        // Build the query
        std::string query = "SELECT * FROM metadata.contract_metadata WHERE 1=1";

        // Execute query
        PublicationTransaction txn(*connection_, publication_transaction_);
        auto result = txn.exec(query);
        txn.commit();

        // Convert to Arrow table
        return convert_metadata_to_arrow(result);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to retrieve contract metadata: " + std::string(e.what()), "PostgresDatabase");
    }
}

std::string PostgresDatabase::asset_class_to_string(AssetClass asset_class) const {
    switch (asset_class) {
        case AssetClass::FUTURES:
            return "FUTURE";
        case AssetClass::EQUITIES:
            return "EQUITY";
        case AssetClass::FIXED_INCOME:
            return "BOND";
        case AssetClass::CURRENCIES:
            return "FOREX";
        case AssetClass::COMMODITIES:
            return "COMMODITY";
        case AssetClass::CRYPTO:
            return "CRYPTO";
        default:
            return "";
    }
}

Result<pqxx::result> PostgresDatabase::execute_market_data_query(
    const std::vector<std::string>& symbols, const Timestamp& start_date, const Timestamp& end_date,
    AssetClass asset_class, DataFrequency freq, const std::string& data_type,
    pqxx::work& txn) {
    // Validate table name components to prevent injection
    auto table_validation = validate_table_name_components(asset_class, data_type, freq);
    if (table_validation.is_error()) {
        return make_error<pqxx::result>(table_validation.error()->code(),
                                        table_validation.error()->what());
    }

    std::string full_table_name = build_table_name(asset_class, data_type, freq);

    if (asset_class == AssetClass::EQUITIES) {
        auto valid=validate_symbols(symbols);
        if(valid.is_error()) return make_error<pqxx::result>(valid.error()->code(),valid.error()->what());
        // Exact pinned-main per-bar adjustment; transaction-local memory only.
        try { txn.exec("SET LOCAL work_mem = '64MB'"); }
        catch(const std::exception& error) {
            WARN("Could not raise work_mem for the equity adjustment query: "+std::string(error.what())+
                 " -- continuing with the session default");
        }
        const auto query=market_data_utils::build_equity_adjusted_query(full_table_name,!symbols.empty());
        const auto first=format_timestamp(start_date),last=format_timestamp(end_date);
        try {
            if(symbols.empty()) return Result<pqxx::result>(txn.exec(query,pqxx::params{first,last}));
            return Result<pqxx::result>(txn.exec(query,pqxx::params{first,last,symbols}));
        } catch(const std::exception& error) {
            return make_error<pqxx::result>(ErrorCode::DATABASE_ERROR,"Query execution failed: "+std::string(error.what()));
        }
    }

    // Base query with parameterized timestamps
    std::string base_query =
        "SELECT time, symbol, open, high, low, close, volume "
        "FROM " +
        full_table_name +
        " "
        "WHERE time BETWEEN $1 AND $2";

    std::string start_ts = format_timestamp(start_date);
    std::string end_ts = format_timestamp(end_date);

    if (symbols.empty()) {
        // No symbol filter
        std::string query = base_query + " ORDER BY time, symbol";
        try {
            return Result<pqxx::result>(txn.exec(query, pqxx::params{start_ts, end_ts}));
        } catch (const std::exception& e) {
            return make_error<pqxx::result>(ErrorCode::DATABASE_ERROR,
                                            "Query execution failed: " + std::string(e.what()));
        }
    } else {
        // With symbol filter - validate symbols first
        auto symbol_validation = validate_symbols(symbols);
        if (symbol_validation.is_error()) {
            return make_error<pqxx::result>(symbol_validation.error()->code(),
                                            symbol_validation.error()->what());
        }

        // Build parameterized query for symbols
        std::string query = base_query + " AND symbol = ANY($3) ORDER BY time, symbol";

        try {
            return Result<pqxx::result>(txn.exec(query, pqxx::params{start_ts, end_ts, symbols}));
        } catch (const std::exception& e) {
            return make_error<pqxx::result>(ErrorCode::DATABASE_ERROR,
                                            "Query execution failed: " + std::string(e.what()));
        }
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::convert_to_arrow_table(
    const pqxx::result& result) {
    if (result.empty()) {
        // Return empty table with schema
        auto schema = arrow::schema(
            {arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
             arrow::field("symbol", arrow::utf8()), arrow::field("open", arrow::float64()),
             arrow::field("high", arrow::float64()), arrow::field("low", arrow::float64()),
             arrow::field("close", arrow::float64()), arrow::field("volume", arrow::float64())});

        std::vector<std::shared_ptr<arrow::Array>> empty_arrays(7);
        auto empty_table = arrow::Table::Make(schema, empty_arrays);

        return Result<std::shared_ptr<arrow::Table>>(empty_table);
    }

    // Create builders for each column
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    arrow::TimestampBuilder timestamp_builder(arrow::timestamp(arrow::TimeUnit::SECOND), pool);
    arrow::StringBuilder symbol_builder(pool);
    arrow::DoubleBuilder open_builder(pool);
    arrow::DoubleBuilder high_builder(pool);
    arrow::DoubleBuilder low_builder(pool);
    arrow::DoubleBuilder close_builder(pool);
    arrow::DoubleBuilder volume_builder(pool);

    // Helpers for error handling
    auto handle_builder_error = [](const std::string& operation) {
        return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::CONVERSION_ERROR,
                                                         "Arrow builder error during " + operation);
    };

    // Reserve space
    try {
        if (timestamp_builder.Reserve(result.size()) != arrow::Status::OK() ||
            symbol_builder.Reserve(result.size()) != arrow::Status::OK() ||
            open_builder.Reserve(result.size()) != arrow::Status::OK() ||
            high_builder.Reserve(result.size()) != arrow::Status::OK() ||
            low_builder.Reserve(result.size()) != arrow::Status::OK() ||
            close_builder.Reserve(result.size()) != arrow::Status::OK() ||
            volume_builder.Reserve(result.size()) != arrow::Status::OK()) {
            return handle_builder_error("reserve");
        }

        // Populate builders
        for (const auto& row : result) {
            // Convert string timestamp to epoch seconds
            std::string time_str = row["time"].as<std::string>();
            std::tm time_info = {};
            std::istringstream ss(time_str);
            ss >> std::get_time(&time_info, "%Y-%m-%d %H:%M:%S");
            time_t time_val = std::mktime(&time_info);
            trade_ngin::core::safe_gmtime(&time_val, &time_info);
            auto tp = std::chrono::system_clock::from_time_t(std::mktime(&time_info));

            auto timestamp =
                std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch()).count();

            // Append values, checking status for each
            if (timestamp_builder.Append(timestamp) != arrow::Status::OK() ||
                symbol_builder.Append(row["symbol"].as<std::string>()) != arrow::Status::OK() ||
                open_builder.Append(row["open"].as<double>()) != arrow::Status::OK() ||
                high_builder.Append(row["high"].as<double>()) != arrow::Status::OK() ||
                low_builder.Append(row["low"].as<double>()) != arrow::Status::OK() ||
                close_builder.Append(row["close"].as<double>()) != arrow::Status::OK() ||
                volume_builder.Append(row["volume"].as<double>()) != arrow::Status::OK()) {
                return handle_builder_error("append");
            }
        }

        // Finish arrays
        std::shared_ptr<arrow::Array> timestamp_array, symbol_array, open_array, high_array,
            low_array, close_array, volume_array;

        if (timestamp_builder.Finish(&timestamp_array) != arrow::Status::OK() ||
            symbol_builder.Finish(&symbol_array) != arrow::Status::OK() ||
            open_builder.Finish(&open_array) != arrow::Status::OK() ||
            high_builder.Finish(&high_array) != arrow::Status::OK() ||
            low_builder.Finish(&low_array) != arrow::Status::OK() ||
            close_builder.Finish(&close_array) != arrow::Status::OK() ||
            volume_builder.Finish(&volume_array) != arrow::Status::OK()) {
            return handle_builder_error("finish");
        }

        // Create schema
        auto schema = arrow::schema(
            {arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
             arrow::field("symbol", arrow::utf8()), arrow::field("open", arrow::float64()),
             arrow::field("high", arrow::float64()), arrow::field("low", arrow::float64()),
             arrow::field("close", arrow::float64()), arrow::field("volume", arrow::float64())});

        // Create and return table
        auto table = arrow::Table::Make(schema, {timestamp_array, symbol_array, open_array,
                                                 high_array, low_array, close_array, volume_array});

        return Result<std::shared_ptr<arrow::Table>>(table);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Exception during Arrow table conversion: " + std::string(e.what()));
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::convert_metadata_to_arrow(
    const pqxx::result& result) const {
    if (result.empty()) {
        // Return empty table with schema
        auto schema = arrow::schema(
            {arrow::field("Name", arrow::utf8()), arrow::field("Databento Symbol", arrow::utf8()),
             arrow::field("IB Symbol", arrow::utf8()), arrow::field("Asset Type", arrow::utf8()),
             arrow::field("Sector", arrow::utf8()), arrow::field("Exchange", arrow::utf8()),
             arrow::field("Contract Size", arrow::float64()),
             arrow::field("Minimum Price Fluctuation", arrow::float64()),
             arrow::field("Tick Size", arrow::utf8()),
             arrow::field("Trading Hours (EST)", arrow::utf8()),
             arrow::field("Overnight Initial Margin", arrow::float64()),
             arrow::field("Overnight Maintenance Margin", arrow::float64()),
             arrow::field("Intraday Initial Margin", arrow::float64()),
             arrow::field("Intraday Maintenance Margin", arrow::float64()),
             arrow::field("Units", arrow::utf8()), arrow::field("Data Provider", arrow::utf8()),
             arrow::field("Dataset", arrow::utf8()),
             arrow::field("Contract Months", arrow::utf8())});

        std::vector<std::shared_ptr<arrow::Array>> empty_arrays;
        empty_arrays.reserve(schema->num_fields());
        for (const auto& field : schema->fields()) {
            auto array = arrow::MakeArrayOfNull(field->type(), 0);
            if (!array.ok()) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::DATABASE_ERROR,
                    "Failed to construct empty contract metadata: " + array.status().ToString(),
                    "PostgresDatabase");
            }
            empty_arrays.push_back(array.ValueOrDie());
        }
        auto empty_table = arrow::Table::Make(schema, empty_arrays);

        return Result<std::shared_ptr<arrow::Table>>(empty_table);
    }

    // Create builders for each column
    arrow::MemoryPool* pool = arrow::default_memory_pool();

    arrow::StringBuilder name_builder(pool);
    arrow::StringBuilder databento_symbol_builder(pool);
    arrow::StringBuilder ib_symbol_builder(pool);
    arrow::StringBuilder asset_type_builder(pool);
    arrow::StringBuilder sector_builder(pool);
    arrow::StringBuilder exchange_builder(pool);
    arrow::DoubleBuilder contract_size_builder(pool);
    arrow::DoubleBuilder min_tick_builder(pool);
    arrow::StringBuilder tick_size_builder(pool);
    arrow::StringBuilder trading_hours_builder(pool);
    arrow::DoubleBuilder overnight_initial_margin_builder(pool);
    arrow::DoubleBuilder overnight_maintenance_margin_builder(pool);
    arrow::DoubleBuilder intraday_initial_margin_builder(pool);
    arrow::DoubleBuilder intraday_maintenance_margin_builder(pool);
    arrow::StringBuilder units_builder(pool);
    arrow::StringBuilder data_provider_builder(pool);
    arrow::StringBuilder dataset_builder(pool);
    arrow::StringBuilder contract_months_builder(pool);

    // Reserve space for builders
    for (auto* builder :
         {&name_builder, &databento_symbol_builder, &ib_symbol_builder, &asset_type_builder,
          &sector_builder, &exchange_builder, &tick_size_builder, &trading_hours_builder,
          &units_builder, &data_provider_builder, &dataset_builder, &contract_months_builder}) {
        auto status = builder->Reserve(result.size());
        if (!status.ok()) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to reserve memory for string builder: " + status.ToString(),
                "PostgresDatabase");
        }
    }

    for (auto* builder : {&contract_size_builder, &min_tick_builder,
                          &overnight_initial_margin_builder, &overnight_maintenance_margin_builder,
                          &intraday_initial_margin_builder, &intraday_maintenance_margin_builder}) {
        auto status = builder->Reserve(result.size());
        if (!status.ok()) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to reserve memory for numeric builder: " + status.ToString(),
                "PostgresDatabase");
        }
    }

    // Define column indices based on debug output
    // Indexes from the debug log: "Databento Symbol, IB Symbol, Name, Exchange, Intraday Initial
    // Margin, ..."
    const int DATABENTO_SYMBOL_IDX = 0;
    const int IB_SYMBOL_IDX = 1;
    const int NAME_IDX = 2;
    const int EXCHANGE_IDX = 3;
    const int INTRADAY_INITIAL_MARGIN_IDX = 4;
    const int INTRADAY_MAINTENANCE_MARGIN_IDX = 5;
    const int OVERNIGHT_INITIAL_MARGIN_IDX = 6;
    const int OVERNIGHT_MAINTENANCE_MARGIN_IDX = 7;
    const int ASSET_TYPE_IDX = 8;
    const int SECTOR_IDX = 9;
    const int CONTRACT_SIZE_IDX = 10;
    const int UNITS_IDX = 11;
    const int MIN_PRICE_FLUCTUATION_IDX = 12;
    const int TICK_SIZE_IDX = 13;
    [[maybe_unused]] const int SETTLEMENT_TYPE_IDX = 14;  // Not needed in final table
    const int TRADING_HOURS_IDX = 15;
    const int DATA_PROVIDER_IDX = 16;
    const int DATASET_IDX = 17;
    [[maybe_unused]] const int NEWEST_MONTH_ADDITIONS_IDX = 18;  // Not needed in final table
    const int CONTRACT_MONTHS_IDX = 19;
    [[maybe_unused]] const int TIME_OF_EXPIRY_IDX = 20;  // Not needed in final table

    // Helper function to safely append string values
    auto append_string = [](arrow::StringBuilder& builder, const pqxx::row& row, int index) {
        try {
            if (index >= row.size() || row[index].is_null()) {
                return builder.AppendNull();
            } else {
                return builder.Append(row[index].c_str());
            }
        } catch (const std::exception& e) {
            ERROR("Exception appending string value: " + std::string(e.what()));
            return builder.AppendNull();
        }
    };

    // Helper function to safely convert and append double values
    auto append_double = [](arrow::DoubleBuilder& builder, const pqxx::row& row, int index) {
        try {
            if (index >= row.size() || row[index].is_null()) {
                return builder.AppendNull();
            } else {
                try {
                    // Convert from text to double
                    std::string str_val = row[index].c_str();
                    double value = 0.0;

                    // Check for empty string
                    if (!str_val.empty()) {
                        try {
                            // Try to convert to double
                            value = std::stod(str_val);
                        } catch (const std::exception&) {
                            // If conversion fails, try to handle some common formats
                            // Remove commas, percentage signs, etc.
                            std::string clean_str = str_val;
                            clean_str.erase(std::remove(clean_str.begin(), clean_str.end(), ','),
                                            clean_str.end());
                            clean_str.erase(std::remove(clean_str.begin(), clean_str.end(), '%'),
                                            clean_str.end());

                            try {
                                value = std::stod(clean_str);
                            } catch (const std::exception&) {
                                // If all conversions fail, append null
                                return builder.AppendNull();
                            }
                        }
                    }

                    return builder.Append(value);
                } catch (const std::exception&) {
                    return builder.AppendNull();
                }
            }
        } catch (const std::exception& e) {
            ERROR("Exception appending double value: " + std::string(e.what()));
            return builder.AppendNull();
        }
    };

    // Populate the arrays
    for (const auto& row : result) {
        try {
            // Append values using the index constants (explicitly ignore return values)
            (void)append_string(name_builder, row, NAME_IDX);
            (void)append_string(databento_symbol_builder, row, DATABENTO_SYMBOL_IDX);
            (void)append_string(ib_symbol_builder, row, IB_SYMBOL_IDX);
            (void)append_string(asset_type_builder, row, ASSET_TYPE_IDX);
            (void)append_string(sector_builder, row, SECTOR_IDX);
            (void)append_string(exchange_builder, row, EXCHANGE_IDX);
            (void)append_double(contract_size_builder, row, CONTRACT_SIZE_IDX);
            (void)append_double(min_tick_builder, row, MIN_PRICE_FLUCTUATION_IDX);
            (void)append_string(tick_size_builder, row, TICK_SIZE_IDX);
            (void)append_string(trading_hours_builder, row, TRADING_HOURS_IDX);
            (void)append_double(overnight_initial_margin_builder, row,
                                OVERNIGHT_INITIAL_MARGIN_IDX);
            (void)append_double(overnight_maintenance_margin_builder, row,
                                OVERNIGHT_MAINTENANCE_MARGIN_IDX);
            (void)append_double(intraday_initial_margin_builder, row, INTRADAY_INITIAL_MARGIN_IDX);
            (void)append_double(intraday_maintenance_margin_builder, row,
                                INTRADAY_MAINTENANCE_MARGIN_IDX);
            (void)append_string(units_builder, row, UNITS_IDX);
            (void)append_string(data_provider_builder, row, DATA_PROVIDER_IDX);
            (void)append_string(dataset_builder, row, DATASET_IDX);
            (void)append_string(contract_months_builder, row, CONTRACT_MONTHS_IDX);
        } catch (const std::exception& e) {
            ERROR("Exception processing row: " + std::string(e.what()));
            // Continue to next row instead of failing completely
            continue;
        }
    }

    // Finish arrays
    std::shared_ptr<arrow::Array> name_array, databento_symbol_array, ib_symbol_array,
        asset_type_array, sector_array, exchange_array, contract_size_array, min_tick_array,
        tick_size_array, trading_hours_array, overnight_initial_margin_array,
        overnight_maintenance_margin_array, intraday_initial_margin_array,
        intraday_maintenance_margin_array, units_array, data_provider_array, dataset_array,
        contract_months_array;

    arrow::Status status;

    // Finish each builder and capture any errors
    status = name_builder.Finish(&name_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'Name' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = databento_symbol_builder.Finish(&databento_symbol_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Databento Symbol' array: " + status.ToString(), "PostgresDatabase");
    }

    status = ib_symbol_builder.Finish(&ib_symbol_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'IB Symbol' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = asset_type_builder.Finish(&asset_type_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Asset Type' array: " + status.ToString(), "PostgresDatabase");
    }

    status = sector_builder.Finish(&sector_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'Sector' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = exchange_builder.Finish(&exchange_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'Exchange' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = contract_size_builder.Finish(&contract_size_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Contract Size' array: " + status.ToString(), "PostgresDatabase");
    }

    status = min_tick_builder.Finish(&min_tick_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Minimum Price Fluctuation' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = tick_size_builder.Finish(&tick_size_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'Tick Size' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = trading_hours_builder.Finish(&trading_hours_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Trading Hours' array: " + status.ToString(), "PostgresDatabase");
    }

    status = overnight_initial_margin_builder.Finish(&overnight_initial_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Overnight Initial Margin' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = overnight_maintenance_margin_builder.Finish(&overnight_maintenance_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Overnight Maintenance Margin' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = intraday_initial_margin_builder.Finish(&intraday_initial_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Intraday Initial Margin' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = intraday_maintenance_margin_builder.Finish(&intraday_maintenance_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Intraday Maintenance Margin' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = units_builder.Finish(&units_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'Units' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = data_provider_builder.Finish(&data_provider_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Data Provider' array: " + status.ToString(), "PostgresDatabase");
    }

    status = dataset_builder.Finish(&dataset_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, "Failed to finish 'Dataset' array: " + status.ToString(),
            "PostgresDatabase");
    }

    status = contract_months_builder.Finish(&contract_months_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Contract Months' array: " + status.ToString(), "PostgresDatabase");
    }

    // Create schema
    auto schema = arrow::schema(
        {arrow::field("Name", arrow::utf8()), arrow::field("Databento Symbol", arrow::utf8()),
         arrow::field("IB Symbol", arrow::utf8()), arrow::field("Asset Type", arrow::utf8()),
         arrow::field("Sector", arrow::utf8()), arrow::field("Exchange", arrow::utf8()),
         arrow::field("Contract Size", arrow::float64()),
         arrow::field("Minimum Price Fluctuation", arrow::float64()),
         arrow::field("Tick Size", arrow::utf8()),
         arrow::field("Trading Hours (EST)", arrow::utf8()),
         arrow::field("Overnight Initial Margin", arrow::float64()),
         arrow::field("Overnight Maintenance Margin", arrow::float64()),
         arrow::field("Intraday Initial Margin", arrow::float64()),
         arrow::field("Intraday Maintenance Margin", arrow::float64()),
         arrow::field("Units", arrow::utf8()), arrow::field("Data Provider", arrow::utf8()),
         arrow::field("Dataset", arrow::utf8()), arrow::field("Contract Months", arrow::utf8())});

    // Create and return table
    auto table = arrow::Table::Make(
        schema,
        {name_array, databento_symbol_array, ib_symbol_array, asset_type_array, sector_array,
         exchange_array, contract_size_array, min_tick_array, tick_size_array, trading_hours_array,
         overnight_initial_margin_array, overnight_maintenance_margin_array,
         intraday_initial_margin_array, intraday_maintenance_margin_array, units_array,
         data_provider_array, dataset_array, contract_months_array});

    return Result<std::shared_ptr<arrow::Table>>(table);
}

std::string PostgresDatabase::side_to_string(Side side) const {
    switch (side) {
        case Side::BUY:
            return "BUY";
        case Side::SELL:
            return "SELL";
        default:
            return "NONE";
    }
}

Result<Timestamp> PostgresDatabase::get_latest_data_time(AssetClass asset_class, DataFrequency freq,
                                                         const std::string& data_type) const {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Timestamp>(validation.error()->code(), validation.error()->what());
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<Timestamp>(table_validation.error()->code(),
                                         table_validation.error()->what());
        }

        std::string query = "SELECT MAX(time) FROM " + full_table_name;
        auto result = txn.exec(query).one_row();

        if (result[0].is_null()) {
            return make_error<Timestamp>(ErrorCode::DATA_NOT_FOUND,
                                         "No data found in " + full_table_name);
        }

        std::string time_str = result[0].as<std::string>();
        std::tm time_info = {};
        std::istringstream ss(time_str);
        ss >> std::get_time(&time_info, "%Y-%m-%d %H:%M:%S");
        time_t time_val = std::mktime(&time_info);
        trade_ngin::core::safe_gmtime(&time_val, &time_info);
        auto tp = std::chrono::system_clock::from_time_t(std::mktime(&time_info));

        return Result<Timestamp>(tp);

    } catch (const std::exception& e) {
        return make_error<Timestamp>(ErrorCode::DATABASE_ERROR,
                                     "Failed to get latest data time: " + std::string(e.what()),
                                     "PostgresDatabase");
    }
}

Result<std::pair<Timestamp, Timestamp>> PostgresDatabase::get_data_time_range(
    AssetClass asset_class, DataFrequency freq, const std::string& data_type) const {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::pair<Timestamp, Timestamp>>(validation.error()->code(),
                                                           validation.error()->what());
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<std::pair<Timestamp, Timestamp>>(table_validation.error()->code(),
                                                               table_validation.error()->what());
        }

        std::string query = "SELECT MIN(time), MAX(time) FROM " + full_table_name;
        auto result = txn.exec(query).one_row();

        if (result[0].is_null() || result[1].is_null()) {
            return make_error<std::pair<Timestamp, Timestamp>>(
                ErrorCode::DATA_NOT_FOUND, "No data found in " + full_table_name);
        }

        auto parse_timestamp = [](const std::string& ts_str) {
            std::tm time_info = {};
            std::istringstream ss(ts_str);
            ss >> std::get_time(&time_info, "%Y-%m-%d %H:%M:%S");
            time_t time_val = std::mktime(&time_info);

            std::tm gm_time_info = {};
            trade_ngin::core::safe_gmtime(&time_val, &gm_time_info);

            return std::chrono::system_clock::from_time_t(std::mktime(&gm_time_info));
        };

        Timestamp start_time = parse_timestamp(result[0].as<std::string>());
        Timestamp end_time = parse_timestamp(result[1].as<std::string>());

        return Result<std::pair<Timestamp, Timestamp>>({start_time, end_time});

    } catch (const std::exception& e) {
        return make_error<std::pair<Timestamp, Timestamp>>(
            ErrorCode::DATABASE_ERROR, "Failed to get data time range: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<size_t> PostgresDatabase::get_data_count(AssetClass asset_class, DataFrequency freq,
                                                const std::string& symbol,
                                                const std::string& data_type) const {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<size_t>(validation.error()->code(), validation.error()->what());
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name components and symbol
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<size_t>(table_validation.error()->code(),
                                      table_validation.error()->what());
        }

        auto symbol_validation = validate_symbol(symbol);
        if (symbol_validation.is_error()) {
            return make_error<size_t>(symbol_validation.error()->code(),
                                      symbol_validation.error()->what());
        }

        std::string query = "SELECT COUNT(*) FROM " + full_table_name + " WHERE symbol = $1";
        auto result = txn.exec(query, pqxx::params{symbol}).one_row();

        return Result<size_t>(result[0].as<size_t>());

    } catch (const std::exception& e) {
        return make_error<size_t>(ErrorCode::DATABASE_ERROR,
                                  "Failed to get data count: " + std::string(e.what()),
                                  "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::validate_table_name_components(AssetClass asset_class,
                                                              const std::string& data_type,
                                                              DataFrequency freq) {
    // Validate data_type contains only alphanumeric and underscore
    if (data_type.empty() || data_type.size() > 50) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid data_type: must be 1-50 characters", "PostgresDatabase");
    }

    for (char c : data_type) {
        if (!std::isalnum(c) && c != '_') {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid data_type: contains non-alphanumeric characters",
                                    "PostgresDatabase");
        }
    }

    // Validate enum values are within range
    if (static_cast<int>(asset_class) < 0 || static_cast<int>(asset_class) > 10) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Invalid asset_class value",
                                "PostgresDatabase");
    }

    if (static_cast<int>(freq) < 0 || static_cast<int>(freq) > 10) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, "Invalid frequency value",
                                "PostgresDatabase");
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_table_name(const std::string& table_name) const {
    if (table_name.empty() || table_name.size() > 100) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid table_name: must be 1-100 characters", "PostgresDatabase");
    }

    // Allow only alphanumeric, underscore, and dot for schema.table format
    for (char c : table_name) {
        if (!std::isalnum(c) && c != '_' && c != '.') {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid table_name: contains invalid characters",
                                    "PostgresDatabase");
        }
    }

    // Prevent SQL injection patterns
    std::string lower_name = table_name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);

    std::vector<std::string> forbidden = {"drop",  "delete", "insert", "update", "alter", "create",
                                          "union", "select", "script", "--",     "/*",    "*/"};

    for (const auto& forbidden_word : forbidden) {
        if (lower_name.find(forbidden_word) != std::string::npos) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid table_name: contains forbidden SQL keywords",
                                    "PostgresDatabase");
        }
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_symbol(const std::string& symbol) {
    if (symbol.empty() || symbol.size() > 20) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid symbol: must be 1-20 characters", "PostgresDatabase");
    }

    // Allow alphanumeric, underscore, dot, and dash for symbols
    for (char c : symbol) {
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid symbol: contains invalid characters",
                                    "PostgresDatabase");
        }
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_symbols(const std::vector<std::string>& symbols) {
    if (symbols.size() > 1000) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Too many symbols: maximum 1000 allowed", "PostgresDatabase");
    }

    for (const auto& symbol : symbols) {
        auto validation = validate_symbol(symbol);
        if (validation.is_error()) {
            return validation;
        }
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_strategy_id(const std::string& strategy_id) const {
    if (strategy_id.empty() || strategy_id.size() > 50) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid strategy_id: must be 1-50 characters", "PostgresDatabase");
    }

    // Allow alphanumeric, underscore, and dash
    for (char c : strategy_id) {
        if (!std::isalnum(c) && c != '_' && c != '-') {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid strategy_id: contains invalid characters",
                                    "PostgresDatabase");
        }
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_execution_report(const ExecutionReport& exec) const {
    // Validate symbol
    auto symbol_validation = validate_symbol(exec.symbol);
    if (symbol_validation.is_error()) {
        return symbol_validation;
    }

    // Validate order_id and exec_id
    if (exec.order_id.empty() || exec.order_id.size() > 50) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid order_id: must be 1-50 characters", "PostgresDatabase");
    }

    if (exec.exec_id.empty() || exec.exec_id.size() > 50) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid exec_id: must be 1-50 characters", "PostgresDatabase");
    }

    // Validate financial values
    if (exec.filled_quantity.is_negative() || static_cast<double>(exec.filled_quantity) > 1e12) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid filled_quantity: must be between 0 and 1e12",
                                "PostgresDatabase");
    }

    if (exec.fill_price.is_negative() || static_cast<double>(exec.fill_price) > 1e12) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid fill_price: must be between 0 and 1e12",
                                "PostgresDatabase");
    }

    if (exec.total_transaction_costs.is_negative() || static_cast<double>(exec.total_transaction_costs) > 1e12) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid total_transaction_costs: must be between 0 and 1e12",
                                "PostgresDatabase");
    }

    constexpr double kMaxNettingAdjustment = 1.0e9;
    const double adjustment = static_cast<double>(exec.netting_adjustment);
    const double net_cost = static_cast<double>(exec.net_transaction_costs());
    if (std::abs(adjustment) > kMaxNettingAdjustment || net_cost < 0.0 || net_cost > 1e12) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid netting_adjustment: absolute value must be at most 1e9 and derived net cost must be non-negative",
            "PostgresDatabase");
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_position(const Position& pos) const {
    // Validate symbol
    auto symbol_validation = validate_symbol(pos.symbol);
    if (symbol_validation.is_error()) {
        return symbol_validation;
    }

    // Validate financial values
    if (pos.quantity.abs() > Decimal(1e9)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid quantity: absolute value must be less than 1e9",
                                "PostgresDatabase");
    }

    if (pos.average_price.is_negative() || pos.average_price > Decimal(1e9)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid average_price: must be between 0 and 1e9",
                                "PostgresDatabase");
    }

    if (pos.unrealized_pnl.abs() > Decimal(1e9) || pos.realized_pnl.abs() > Decimal(1e9)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid PnL values: absolute value must be less than 1e9",
                                "PostgresDatabase");
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_signal_data(const std::string& symbol,
                                                    double signal) const {
    // Validate symbol
    auto symbol_validation = validate_symbol(symbol);
    if (symbol_validation.is_error()) {
        return symbol_validation;
    }

    // Validate signal value
    if (std::isnan(signal) || std::isinf(signal)) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid signal: contains NaN or infinity value",
                                "PostgresDatabase");
    }

    if (std::abs(signal) > 1e6) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid signal: absolute value must be less than 1e6",
                                "PostgresDatabase");
    }

    return Result<void>();
}

// ============================================================================
// BACKTEST DATA STORAGE IMPLEMENTATIONS
// ============================================================================

Result<void> PostgresDatabase::store_backtest_executions(
    const std::vector<ExecutionReport>& executions, const std::string& run_id,
    const std::string& portfolio_id, const std::string& table_name) {
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        const std::string& actual_portfolio_id = portfolio_id;

        // Chunked, fully-parameterized batch insert: every value is bound, never
        // concatenated. Chunk size keeps rows*cols comfortably under Postgres's
        // 65535-parameter-per-query limit while still batching most round trips away.
        constexpr size_t kCols = 14;
        constexpr size_t kChunkSize = 1000;  // 1000 * 14 = 14000 params, well under the cap
        std::string insert_prefix =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, execution_id, order_id, timestamp, "
            "symbol, side, quantity, price, commissions_fees, "
            "implicit_price_impact, slippage_market_impact, "
            "total_transaction_costs, is_partial) VALUES ";

        for (size_t start = 0; start < executions.size(); start += kChunkSize) {
            size_t end = std::min(start + kChunkSize, executions.size());
            size_t chunk_rows = end - start;

            pqxx::params params;
            for (size_t i = start; i < end; ++i) {
                const auto& exec = executions[i];
                params.append(run_id);
                params.append(actual_portfolio_id);
                params.append(exec.exec_id);
                params.append(exec.order_id);
                params.append(format_timestamp(exec.fill_time));
                params.append(exec.symbol);
                params.append(side_to_string(exec.side));
                params.append(static_cast<double>(exec.filled_quantity));
                params.append(static_cast<double>(exec.fill_price));
                params.append(static_cast<double>(exec.commissions_fees));
                params.append(static_cast<double>(exec.implicit_price_impact));
                params.append(static_cast<double>(exec.slippage_market_impact));
                params.append(static_cast<double>(exec.total_transaction_costs));
                params.append(exec.is_partial);
            }

            std::string query = insert_prefix + build_value_placeholders(chunk_rows, kCols);
            txn.exec(query, params);
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(executions.size()) +
             " backtest executions for run: " + run_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest executions: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_executions_with_strategy(
    const std::vector<ExecutionReport>& executions, const std::string& run_id,
    const std::string& strategy_id, const std::string& portfolio_id,
    const std::string& table_name) {
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        const std::string& actual_portfolio_id = portfolio_id;

        // Chunked, fully-parameterized batch insert -- see store_backtest_executions above
        // for why chunking (not one unbounded query) is required.
        constexpr size_t kCols = 15;
        constexpr size_t kChunkSize = 1000;  // 1000 * 15 = 15000 params, well under the cap
        std::string insert_prefix =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, strategy_id, execution_id, order_id, timestamp, symbol, "
            "side, quantity, price, commissions_fees, implicit_price_impact, "
            "slippage_market_impact, total_transaction_costs, is_partial) VALUES ";

        for (size_t start = 0; start < executions.size(); start += kChunkSize) {
            size_t end = std::min(start + kChunkSize, executions.size());
            size_t chunk_rows = end - start;

            pqxx::params params;
            for (size_t i = start; i < end; ++i) {
                const auto& exec = executions[i];
                params.append(run_id);
                params.append(actual_portfolio_id);
                params.append(strategy_id);
                params.append(exec.exec_id);
                params.append(exec.order_id);
                params.append(format_timestamp(exec.fill_time));
                params.append(exec.symbol);
                params.append(side_to_string(exec.side));
                params.append(static_cast<double>(exec.filled_quantity));
                params.append(static_cast<double>(exec.fill_price));
                params.append(static_cast<double>(exec.commissions_fees));
                params.append(static_cast<double>(exec.implicit_price_impact));
                params.append(static_cast<double>(exec.slippage_market_impact));
                params.append(static_cast<double>(exec.total_transaction_costs));
                params.append(exec.is_partial);
            }

            std::string query = insert_prefix + build_value_placeholders(chunk_rows, kCols);
            txn.exec(query, params);
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(executions.size()) +
             " backtest executions for run: " + run_id + ", strategy_id: " + strategy_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store backtest executions with strategy: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_signals(
    const std::unordered_map<std::string, double>& signals, const std::string& strategy_id,
    const std::string& run_id, const Timestamp& timestamp, const std::string& portfolio_id,
    const std::string& table_name) {
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        const std::string& actual_portfolio_id = portfolio_id;

        for (const auto& [symbol, signal_value] : signals) {
            // For backtest.signals, include portfolio_run_id if run_id looks like a portfolio
            // run_id (contains '&') Schema has portfolio_run_id column (nullable) and portfolio_id
            // column
            std::string query;
            if (table_name == "backtest.signals" && run_id.find('&') != std::string::npos) {
                // Portfolio run: run_id is portfolio_run_id format, use it for portfolio_run_id
                // column
                query = "INSERT INTO " + table_name +
                        " (run_id, portfolio_id, strategy_id, symbol, signal_value, timestamp, "
                        "portfolio_run_id) "
                        "VALUES ($1, $2, $3, $4, $5, $6, $7) "
                        "ON CONFLICT (run_id, strategy_id, symbol, timestamp) "
                        "DO UPDATE SET signal_value = EXCLUDED.signal_value, portfolio_id = "
                        "EXCLUDED.portfolio_id, portfolio_run_id = EXCLUDED.portfolio_run_id";
                txn.exec(query, pqxx::params{run_id, actual_portfolio_id, strategy_id, symbol,
                                signal_value, format_timestamp(timestamp), run_id});
            } else {
                // Single strategy run or other table: include portfolio_id
                query = "INSERT INTO " + table_name +
                        " (run_id, portfolio_id, strategy_id, symbol, signal_value, timestamp) "
                        "VALUES ($1, $2, $3, $4, $5, $6) "
                        "ON CONFLICT (run_id, strategy_id, symbol, timestamp) "
                        "DO UPDATE SET signal_value = EXCLUDED.signal_value, portfolio_id = "
                        "EXCLUDED.portfolio_id";
                txn.exec(query, pqxx::params{run_id, actual_portfolio_id, strategy_id, symbol,
                                signal_value, format_timestamp(timestamp)});
            }
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(signals.size()) +
             " backtest signals for strategy: " + strategy_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest signals: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_metadata(
    const std::string& run_id, const std::string& name, const std::string& description,
    const Timestamp& start_date, const Timestamp& end_date, const nlohmann::json& hyperparameters,
    const std::string& portfolio_id, const std::string& table_name) {
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        const std::string& actual_portfolio_id = portfolio_id;

        std::string query =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, name, description, start_date, end_date, hyperparameters) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7) "
            "ON CONFLICT (run_id) "
            "DO UPDATE SET portfolio_id = EXCLUDED.portfolio_id, name = EXCLUDED.name, description "
            "= EXCLUDED.description, "
            "start_date = EXCLUDED.start_date, end_date = EXCLUDED.end_date, "
            "hyperparameters = EXCLUDED.hyperparameters";

        txn.exec(query, pqxx::params{run_id, actual_portfolio_id, name, description,
                        format_timestamp(start_date), format_timestamp(end_date),
                        hyperparameters.dump()});

        txn.commit();
        INFO("Successfully stored backtest metadata for run: " + run_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest metadata: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_metadata_with_portfolio(
    const std::string& run_id, const std::string& portfolio_run_id, const std::string& strategy_id,
    double strategy_allocation, const nlohmann::json& portfolio_config, const std::string& name,
    const std::string& description, const Timestamp& start_date, const Timestamp& end_date,
    const nlohmann::json& hyperparameters, const std::string& portfolio_id,
    const std::string& table_name) {
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) return portfolio_validation;
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        const std::string& actual_portfolio_id = portfolio_id;

        std::string query =
            "INSERT INTO " + table_name +
            " (run_id, portfolio_id, portfolio_run_id, strategy_id, strategy_allocation, "
            "portfolio_config, name, description, start_date, end_date, hyperparameters) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11) "
            "ON CONFLICT (run_id, strategy_id) "
            "DO UPDATE SET portfolio_id = EXCLUDED.portfolio_id, portfolio_run_id = "
            "EXCLUDED.portfolio_run_id, "
            "strategy_allocation = EXCLUDED.strategy_allocation, "
            "portfolio_config = EXCLUDED.portfolio_config, "
            "name = EXCLUDED.name, description = EXCLUDED.description, "
            "start_date = EXCLUDED.start_date, end_date = EXCLUDED.end_date, "
            "hyperparameters = EXCLUDED.hyperparameters";

        txn.exec(query, pqxx::params{run_id, actual_portfolio_id, portfolio_run_id, strategy_id,
                        strategy_allocation, portfolio_config.dump(), name, description,
                        format_timestamp(start_date), format_timestamp(end_date),
                        hyperparameters.dump()});

        txn.commit();
        INFO("Successfully stored backtest metadata with portfolio for run: " + run_id +
             ", portfolio_run_id: " + portfolio_run_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store backtest metadata with portfolio: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

// ============================================================================
// LIVE TRADING DATA STORAGE IMPLEMENTATIONS
// ============================================================================

Result<void> PostgresDatabase::store_trading_results(
    const std::string& strategy_id, const Timestamp& date, double total_return, double sharpe_ratio,
    double sortino_ratio, double max_drawdown, double calmar_ratio, double volatility,
    int total_trades, double win_rate, double profit_factor, double avg_win, double avg_loss,
    double max_win, double max_loss, double avg_holding_period, double var_95, double cvar_95,
    double beta, double correlation, double downside_volatility, const nlohmann::json& config,
    const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        std::string query =
            "INSERT INTO " + table_name +
            " (strategy_id, date, total_return, sharpe_ratio, sortino_ratio, max_drawdown, "
            "calmar_ratio, volatility, total_trades, win_rate, profit_factor, avg_win, avg_loss, "
            "max_win, max_loss, avg_holding_period, var_95, cvar_95, beta, correlation, "
            "downside_volatility, config) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, "
            "$18, $19, $20, $21, $22) "
            "ON CONFLICT (strategy_id, date) "
            "DO UPDATE SET total_return = EXCLUDED.total_return, sharpe_ratio = "
            "EXCLUDED.sharpe_ratio, "
            "sortino_ratio = EXCLUDED.sortino_ratio, max_drawdown = EXCLUDED.max_drawdown, "
            "calmar_ratio = EXCLUDED.calmar_ratio, volatility = EXCLUDED.volatility, "
            "total_trades = EXCLUDED.total_trades, win_rate = EXCLUDED.win_rate, "
            "profit_factor = EXCLUDED.profit_factor, avg_win = EXCLUDED.avg_win, "
            "avg_loss = EXCLUDED.avg_loss, max_win = EXCLUDED.max_win, max_loss = "
            "EXCLUDED.max_loss, "
            "avg_holding_period = EXCLUDED.avg_holding_period, var_95 = EXCLUDED.var_95, "
            "cvar_95 = EXCLUDED.cvar_95, beta = EXCLUDED.beta, correlation = EXCLUDED.correlation, "
            "downside_volatility = EXCLUDED.downside_volatility, config = EXCLUDED.config";

        txn.exec(query, pqxx::params{strategy_id, format_timestamp(date), total_return, sharpe_ratio,
                        sortino_ratio, max_drawdown, calmar_ratio, volatility, total_trades,
                        win_rate, profit_factor, avg_win, avg_loss, max_win, max_loss,
                        avg_holding_period, var_95, cvar_95, beta, correlation, downside_volatility,
                        config.dump()});

        txn.commit();
        INFO("Successfully stored trading results for strategy: " + strategy_id + " on " +
             format_timestamp(date));
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store trading results: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_live_results(
    const std::string& strategy_id, const Timestamp& date, double total_return, double volatility,
    double total_pnl, double unrealized_pnl, double realized_pnl, double current_portfolio_value,
    double daily_realized_pnl, double daily_unrealized_pnl, double portfolio_var,
    double net_leverage, double gross_leverage, double margin_leverage,
    double margin_cushion, double max_correlation, double jump_risk, double risk_scale,
    double gross_notional, double net_notional, int active_positions, double total_transaction_costs,
    double margin_posted, double cash_available, const nlohmann::json& config,
    const std::string& table_name, const std::string& portfolio_id, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    if (pending_publication_ && !publication_transaction_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_generic_summary_refused");
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        // Note: gross_leverage C++ param maps to portfolio_leverage DB column
        // The old gross_leverage DB column is no longer written to
        std::string query =
            "INSERT INTO " + table_name +
            " (strategy_id, date, total_return, volatility, total_pnl, total_unrealized_pnl, "
            "total_realized_pnl, "
            "current_portfolio_value, daily_realized_pnl, daily_unrealized_pnl, portfolio_var, "
            "net_leverage, portfolio_leverage, margin_leverage, margin_cushion, "
            "max_correlation, jump_risk, "
            "risk_scale, gross_notional, net_notional, active_positions, total_transaction_costs, "
            "margin_posted, cash_available, config, portfolio_id, portfolio_type) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, "
            "$17, $18, $19, $20, $21, $22, $23, $24, $25, $26, $27) "
            "ON CONFLICT (portfolio_id, strategy_id, date, portfolio_type) "
            "DO UPDATE SET total_return = EXCLUDED.total_return, volatility = EXCLUDED.volatility, "
            "total_pnl = EXCLUDED.total_pnl, total_unrealized_pnl = EXCLUDED.total_unrealized_pnl, "
            "total_realized_pnl = EXCLUDED.total_realized_pnl, current_portfolio_value = "
            "EXCLUDED.current_portfolio_value, "
            "daily_realized_pnl = EXCLUDED.daily_realized_pnl, daily_unrealized_pnl = "
            "EXCLUDED.daily_unrealized_pnl, "
            "portfolio_var = EXCLUDED.portfolio_var, "
            "net_leverage = EXCLUDED.net_leverage, portfolio_leverage = "
            "EXCLUDED.portfolio_leverage, margin_leverage = EXCLUDED.margin_leverage, "
            "margin_cushion = EXCLUDED.margin_cushion, "
            "max_correlation = EXCLUDED.max_correlation, jump_risk = EXCLUDED.jump_risk, "
            "risk_scale = EXCLUDED.risk_scale, gross_notional = EXCLUDED.gross_notional, "
            "net_notional = EXCLUDED.net_notional, active_positions = EXCLUDED.active_positions, "
            "total_transaction_costs = EXCLUDED.total_transaction_costs, margin_posted = "
            "EXCLUDED.margin_posted, cash_available = EXCLUDED.cash_available, config = "
            "EXCLUDED.config";

        txn.exec(query, pqxx::params{strategy_id, format_timestamp(date), total_return, volatility,
                        total_pnl, unrealized_pnl, realized_pnl, current_portfolio_value,
                        daily_realized_pnl, daily_unrealized_pnl, portfolio_var,
                        net_leverage, gross_leverage, margin_leverage, margin_cushion,
                        max_correlation, jump_risk, risk_scale, gross_notional, net_notional,
                        active_positions, total_transaction_costs, margin_posted, cash_available,
                        config.dump(),portfolio_id,portfolio_type});

        txn.commit();
        INFO("Successfully stored live results for strategy: " + strategy_id + " on " +
             format_timestamp(date));
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store live results: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<std::tuple<double, double, double>> PostgresDatabase::get_previous_live_aggregates(
    const std::string& strategy_id, const std::string& portfolio_id, const Timestamp& date,
    const std::string& table_name, const std::string& portfolio_type) {
    if (portfolio_type == "qt_proposal") poison_proposal_refusal();
    auto portfolio_validation = validate_portfolio_id(portfolio_id);
    if (portfolio_validation.is_error()) {
        return make_error<std::tuple<double, double, double>>(
            portfolio_validation.error()->code(), portfolio_validation.error()->what());
    }
    if (portfolio_type != "system" && portfolio_type != "qt")
        return make_error<std::tuple<double,double,double>>(ErrorCode::INVALID_ARGUMENT,"unsupported_stream");
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::tuple<double, double, double>>(validation.error()->code(),
                                                              validation.error()->what());
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return make_error<std::tuple<double, double, double>>(table_validation.error()->code(),
                                                                  table_validation.error()->what());
        }

        const std::string& actual_portfolio_id = portfolio_id;

        // Build query for the most recent previous record before the current date
        // Instead of strict "1 day ago", we look for the latest record with date < current_date
        // This handles weekends and holidays correctly
        std::string query =
            "SELECT current_portfolio_value, total_pnl, total_transaction_costs "
            "FROM " +
            table_name +
            " WHERE strategy_id = $1 AND portfolio_id = $2 AND DATE(date) < DATE($3) AND portfolio_type=$4 "
            "ORDER BY date DESC, created_at DESC LIMIT 1";

        auto result =
            txn.exec(query, pqxx::params{strategy_id, actual_portfolio_id, format_timestamp(date),portfolio_type});
        txn.commit();

        if (result.empty()) {
            // Return error if no previous data found - caller should handle this
            return make_error<std::tuple<double, double, double>>(
                ErrorCode::DATABASE_ERROR, "No previous aggregates found for strategy " +
                                               strategy_id + " (portfolio: " + actual_portfolio_id +
                                               ")");
        }

        double prev_value = 0.0;
        double prev_total_pnl = 0.0;
        double prev_total_transaction_costs = 0.0;

        const auto& row = result[0];
        if (!row[0].is_null())
            prev_value = row[0].as<double>();
        if (!row[1].is_null())
            prev_total_pnl = row[1].as<double>();
        if (!row[2].is_null())
            prev_total_transaction_costs = row[2].as<double>();

        return Result<std::tuple<double, double, double>>(
            std::make_tuple(prev_value, prev_total_pnl, prev_total_transaction_costs));
    } catch (const std::exception& e) {
        return make_error<std::tuple<double, double, double>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch previous live aggregates: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<int> PostgresDatabase::seed_qt_positions_from_system(const std::string& strategy_id,
                                                           const std::string& strategy_name,
                                                           const std::string& portfolio_id,
                                                           const std::string& date,
                                                           const std::string& table_name) {
    if (pending_publication_ && pending_publication_->date == date &&
        defer_live_write([this,strategy_id,strategy_name,portfolio_id,date,table_name]() {
            auto seeded = seed_qt_positions_from_system(strategy_id,strategy_name,portfolio_id,date,table_name);
            if (seeded.is_error()) return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_qt_seed_failed");
            return Result<void>();
        }, QtSeedPart)) return Result<int>(0);
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<int>(validation.error()->code(), validation.error()->what());
    }

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,"qt");


        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return make_error<int>(table_validation.error()->code(),
                                   table_validation.error()->what());
        }

        if (!column_exists(txn, table_name, "portfolio_type")) {
            WARN("Cannot seed qt positions: " + table_name +
                 " has no portfolio_type column (migration 001 not applied). Skipping.");
            return Result<int>(0);
        }

        // QT current state wins over system proposals across dates, including
        // zero closures and symbols the system no longer proposes. New symbols
        // are seeded from today's system only when no QT state exists. The
        // unique-key conflict guard preserves same-day/concurrent QT edits.
        std::string query =
            "WITH candidates AS (SELECT *, ROW_NUMBER() OVER (PARTITION BY symbol "
            "ORDER BY CASE WHEN portfolio_type = 'qt' THEN 0 ELSE 1 END, date DESC) "
            "AS qt_rank FROM " + table_name +
            " WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3 "
            "AND ((portfolio_type = 'qt' AND date <= $4) "
            "OR (portfolio_type = 'system' AND date = $4))) "
            "INSERT INTO " + table_name +
            " (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, "
            " last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, "
            " portfolio_type) "
            "SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, "
            "       last_update, updated_at, strategy_id, strategy_name, $4, portfolio_id, "
            "       'qt' "
            "FROM candidates WHERE qt_rank = 1 "
            "ON CONFLICT (portfolio_id, strategy_id, strategy_name, date, symbol, "
            "portfolio_type) DO NOTHING";

        auto result = txn.exec(query, pqxx::params{strategy_id, strategy_name, portfolio_id, date});
        txn.commit();

        const int seeded = static_cast<int>(result.affected_rows());
        if (seeded > 0) {
            INFO("Seeded " + std::to_string(seeded) + " qt position(s) from system for " +
                 strategy_name + " on " + date);
        } else {
            INFO("qt stream already present for " + strategy_name + " on " + date +
                 " -- leaving existing QT edits untouched");
        }
        return Result<int>(seeded);

    } catch (const std::exception& e) {
        return make_error<int>(ErrorCode::DATABASE_ERROR,
                               "Failed to seed qt positions: " + std::string(e.what()),
                               "PostgresDatabase");
    }
}

Result<int> PostgresDatabase::seed_qt_proposal_positions_from_system(
    const std::string& strategy_id, const std::string& strategy_name,
    const std::string& portfolio_id, const std::string& date) {
    if (!nonblank_proposal_identity(strategy_id) ||
        !nonblank_proposal_identity(strategy_name) ||
        !nonblank_proposal_identity(portfolio_id) || !valid_proposal_day(date)) {
        poison_proposal_refusal();
        return make_error<int>(ErrorCode::INVALID_ARGUMENT,"proposal_scope_invalid");
    }
    if (pending_publication_ && !publication_transaction_) {
        const auto& scope = *pending_publication_;
        const auto& configured = scope.snapshot.at("strategies");
        if (scope.date != date || scope.strategy_id != strategy_id ||
            scope.portfolio_id != portfolio_id || !configured.contains(strategy_name) ||
            !configured.at(strategy_name).value("enabled_live",false) ||
            std::find(scope.fresh_system_members.begin(),scope.fresh_system_members.end(),strategy_name)
                == scope.fresh_system_members.end()) {
            poison_proposal_refusal();
            return make_error<int>(ErrorCode::INVALID_ARGUMENT,"proposal_pending_scope_or_order_invalid");
        }
        try {
            auto validation = validate_connection();
            if (validation.is_error()) throw std::runtime_error("proposal_database_unavailable");
            pqxx::work preflight(*connection_);
            preflight.exec("SET TRANSACTION READ ONLY");
            require_proposal_capability(preflight);
            preflight.commit();
        } catch (const std::exception& e) {
            poison_proposal_refusal();
            return make_error<int>(ErrorCode::DATABASE_ERROR,
                "proposal015_schema_unavailable: " + std::string(e.what()));
        }
        if (!pending_publication_->proposal_sealed_members.contains(strategy_name)) {
            pending_publication_->writes.push_back([this,strategy_id,strategy_name,portfolio_id,date]() {
                const auto seeded = seed_qt_proposal_positions_from_system(
                    strategy_id,strategy_name,portfolio_id,date);
                if (seeded.is_error())
                    return make_error<void>(ErrorCode::DATABASE_ERROR,"runtime_proposal_seed_failed");
                return Result<void>();
            });
            pending_publication_->proposal_sealed_members.insert(strategy_name);
        }
        return Result<int>(0);
    }
    auto validation = validate_connection();
    if (validation.is_error()) return make_error<int>(validation.error()->code(),validation.error()->what());
    try {
        PublicationTransaction txn(*connection_,publication_transaction_);
        require_proposal_capability(txn);
        fence_live_write(txn,strategy_id,portfolio_id,"qt_proposal",true);
        const bool capture_insertions = publication_transaction_ && pending_publication_;
        std::string insert_sql =
            "INSERT INTO trading.positions (symbol,quantity,average_price,daily_unrealized_pnl,"
            "daily_realized_pnl,last_update,updated_at,strategy_id,strategy_name,date,"
            "portfolio_id,portfolio_type) "
            "SELECT s.symbol,s.quantity,s.average_price,s.daily_unrealized_pnl,s.daily_realized_pnl,"
            "s.last_update,s.updated_at,s.strategy_id,s.strategy_name,s.date,s.portfolio_id,'qt_proposal' "
            "FROM trading.positions s WHERE s.strategy_id=$1 AND s.strategy_name=$2 "
            "AND s.portfolio_id=$3 AND s.date=$4::date AND s.portfolio_type='system' "
            "AND NOT EXISTS (SELECT 1 FROM trading.positions p WHERE p.portfolio_id=s.portfolio_id "
            "AND p.strategy_id=s.strategy_id AND p.strategy_name=s.strategy_name AND p.date=s.date "
            "AND p.symbol=s.symbol AND p.portfolio_type='qt_proposal') "
            "ON CONFLICT (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type) DO NOTHING";
        if (capture_insertions)
            insert_sql += " RETURNING portfolio_id,strategy_id,strategy_name,date::text,"
                          "symbol,portfolio_type,qt_proposal_revision::text";
        const auto result = txn.exec(insert_sql,
            pqxx::params{strategy_id,strategy_name,portfolio_id,date});
        const int count = static_cast<int>(capture_insertions ? result.size()
                                                             : result.affected_rows());
        if (capture_insertions) {
            for (const auto& row : result) {
                if (row[6].is_null()) throw std::runtime_error("proposal_insert_revision_missing");
                pending_publication_->inserted_proposal_revisions.emplace_back(
                    ComponentPositionKey{row[0].as<std::string>(),row[1].as<std::string>(),
                        row[2].as<std::string>(),row[3].as<std::string>(),
                        row[4].as<std::string>(),row[5].as<std::string>()},
                    row[6].as<std::string>());
            }
        }
        txn.commit();
        return Result<int>(count);
    } catch (const std::exception& e) {
        return make_error<int>(ErrorCode::DATABASE_ERROR,
            "proposal_seed_failed: " + std::string(e.what()));
    }
}

Result<void> PostgresDatabase::record_qt_model_seed_publication(
    const QtModelSeedPublication& publication) {
    if (!pending_publication_ || !publication_transaction_ ||
        publication.publication_id != pending_publication_->publication_id ||
        publication.portfolio_id != pending_publication_->portfolio_id ||
        publication.strategy_id != pending_publication_->strategy_id ||
        publication.source_day != pending_publication_->date ||
        publication.producer_version != pending_publication_->producer_version)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "qt_model_seed_requires_current_publication");
    try {
        auto& txn = *publication_transaction_;
        require_qt_exact_storage_capability(txn);
        const bool empty_owner=publication.strategy_id=="LIVE_EQUITY_MEAN_REVERSION" && publication.system_components.empty() &&
            !pending_publication_->fresh_empty_batches.empty();
        auto document = empty_owner ? Result<nlohmann::json>(nlohmann::json{{"seed_rows",nlohmann::json::array()}}) : qt_model_seed_document(publication);
        auto digest = empty_owner ? qt_digest_v1(document.value()) : qt_model_seed_digest(publication);
        if (document.is_error() || digest.is_error() ||
            digest.value() != publication.seed_digest)
            throw std::runtime_error("qt_model_seed_digest_invalid");

        auto expected = publication.system_components;
        std::sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) {
            return a.key < b.key;
        });
        auto actual = txn.exec(
            "SELECT portfolio_id,strategy_id,strategy_name,date::text,symbol,portfolio_type,"
            "quantity::text,average_price::text FROM trading.positions "
            "WHERE portfolio_id=$1 AND strategy_id=$2 AND date=$3::date "
            "AND portfolio_type='system' "
            "ORDER BY portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type",
            pqxx::params{publication.portfolio_id,publication.strategy_id,
                         publication.source_day});
        if (static_cast<size_t>(actual.size()) != expected.size())
            throw std::runtime_error("qt_model_seed_source_set_changed");
        for (size_t i = 0; i < expected.size(); ++i) {
            const auto& row = actual[i];
            const ComponentPositionKey key{row[0].as<std::string>(),row[1].as<std::string>(),
                row[2].as<std::string>(),row[3].as<std::string>(),
                row[4].as<std::string>(),row[5].as<std::string>()};
            if (!(key == expected[i].key) ||
                exact_position_decimal(row[6].as<std::string>()) != expected[i].quantity ||
                exact_position_decimal(row[7].as<std::string>()) != expected[i].average_price)
                throw std::runtime_error("qt_model_seed_source_value_changed");
        }
        QtModelSeedPublication recorded = publication;
        const auto proposals = txn.exec(
            "SELECT portfolio_id,strategy_id,strategy_name,date::text,symbol,portfolio_type,"
            "quantity::text,average_price::text,qt_proposal_revision::text "
            "FROM trading.positions WHERE portfolio_id=$1 AND strategy_id=$2 "
            "AND date=$3::date AND portfolio_type='qt_proposal'",
            pqxx::params{publication.portfolio_id,publication.strategy_id,
                         publication.source_day});
        const bool v2_capability=require_qt_empty_owner_storage_capability(txn);
        const std::string prior_query = v2_capability ?
            "SELECT publication_id::text,strategy_id,proposal_components::text,proposal_manifest_digest FROM (SELECT publication_id,strategy_id,proposal_components,proposal_manifest_digest,publication_version FROM trading.qt_model_seed_publications WHERE portfolio_id=$1 AND source_day=$2::date UNION ALL SELECT publication_id,strategy_id,proposal_components,proposal_manifest_digest,publication_version FROM trading.qt_empty_model_owner_publications WHERE portfolio_id=$1 AND source_day=$2::date) p ORDER BY publication_version" :
            "SELECT publication_id::text,strategy_id,proposal_components::text,proposal_manifest_digest FROM trading.qt_model_seed_publications WHERE portfolio_id=$1 AND source_day=$2::date ORDER BY publication_version";
        const auto prior = txn.exec(prior_query,
            pqxx::params{publication.portfolio_id,publication.source_day});
        std::vector<std::pair<std::string,nlohmann::json>> prior_insertions;
        for (const auto& earlier : prior) {
            if (earlier[2].is_null() || earlier[3].is_null()) continue;
            const auto entries = nlohmann::json::parse(earlier[2].as<std::string>());
            auto verified = qt_validate_proposal_manifest_json(entries,
                publication.portfolio_id,earlier[1].as<std::string>(),
                publication.source_day);
            if (verified.is_error())
                throw std::runtime_error("qt_prior_proposal_manifest_invalid");
            const auto bytes = verified.value().dump(-1,' ',false,
                nlohmann::json::error_handler_t::strict);
            auto digest = qt_sha256_hex(bytes);
            if (digest.is_error() || digest.value()!=earlier[3].as<std::string>())
                throw std::runtime_error("qt_prior_proposal_manifest_digest_invalid");
            prior_insertions.emplace_back(earlier[0].as<std::string>(),
                                          std::move(verified.value().at("proposal_rows")));
        }
        std::set<size_t> seen_inserted;
        for (const auto& row : proposals) {
            QtProposalManifestRow item{
                ComponentPositionKey{row[0].as<std::string>(),row[1].as<std::string>(),
                    row[2].as<std::string>(),row[3].as<std::string>(),
                    row[4].as<std::string>(),row[5].as<std::string>()},
                exact_position_decimal(row[6].as<std::string>()),
                exact_position_decimal(row[7].as<std::string>()),
                "preserved",row[8].is_null() ? std::optional<std::string>{}
                                            : std::optional<std::string>{row[8].as<std::string>()},
                std::nullopt};
            for (size_t i=0;i<pending_publication_->inserted_proposal_revisions.size();++i) {
                const auto& inserted=pending_publication_->inserted_proposal_revisions[i];
                if (!(inserted.first==item.key)) continue;
                if (!item.position_revision || *item.position_revision!=inserted.second ||
                    !seen_inserted.insert(i).second)
                    throw std::runtime_error("qt_proposal_insert_readback_changed");
                item.action="inserted";
                item.origin_publication_id=publication.publication_id;
                break;
            }
            if (item.action=="preserved" && item.position_revision) {
                const nlohmann::json key{{"portfolio_id",item.key.portfolio_id},
                    {"strategy_id",item.key.strategy_id},
                    {"strategy_name",item.key.strategy_name},{"date",item.key.date},
                    {"symbol",item.key.symbol},{"portfolio_type",item.key.portfolio_type}};
                size_t links=0;
                std::string origin;
                for (const auto& [prior_id,entries] : prior_insertions) {
                    for (const auto& entry : entries) {
                        if (entry.at("action")!="inserted" || entry.at("key")!=key ||
                            entry.at("position_revision")!=*item.position_revision ||
                            entry.at("quantity_exact")!=item.quantity.to_string() ||
                            entry.at("average_price_exact")!=item.average_price.to_string() ||
                            entry.at("origin_publication_id")!=prior_id) continue;
                        ++links;
                        origin=prior_id;
                    }
                }
                if (links==1) item.origin_publication_id=origin;
            }
            recorded.proposal_components.push_back(std::move(item));
        }
        if (seen_inserted.size()!=pending_publication_->inserted_proposal_revisions.size())
            throw std::runtime_error("qt_proposal_insert_set_changed");
        const auto manifest=qt_proposal_manifest_document(recorded);
        const auto manifest_digest=qt_proposal_manifest_digest(recorded);
        if (manifest.is_error() || manifest_digest.is_error())
            throw std::runtime_error("qt_proposal_manifest_invalid");
        const std::string version_query=v2_capability ?
            "SELECT COALESCE(MAX(publication_version),0)+1 FROM (SELECT publication_version FROM trading.qt_model_seed_publications WHERE portfolio_id=$1 AND source_day=$2::date UNION ALL SELECT publication_version FROM trading.qt_empty_model_owner_publications WHERE portfolio_id=$1 AND source_day=$2::date) p" :
            "SELECT COALESCE(MAX(publication_version),0)+1 FROM trading.qt_model_seed_publications WHERE portfolio_id=$1 AND source_day=$2::date";
        const auto version = txn.exec(version_query,
            pqxx::params{publication.portfolio_id,
                         publication.source_day})[0][0].as<long long>();
        if(empty_owner) {
            if(!v2_capability || pending_publication_->evidence_requirement!=PublicationEvidenceRequirement::RequiredFinalObservations ||
                !pending_publication_->inspection_capture_queued || !pending_publication_->equity_final_consumption)
                throw std::runtime_error("runtime_empty_owner_evidence_incomplete");
            std::vector<std::string> expected_owners;
            for(const auto& [name,definition]:pending_publication_->snapshot.at("strategies").items())
                if(definition.value("enabled_live",false))expected_owners.push_back(name);
            std::sort(expected_owners.begin(),expected_owners.end());
            const auto inventory=txn.exec("SELECT strategy_id,strategy_name,portfolio_type FROM trading.positions WHERE portfolio_id=$1 AND date=$2::date AND portfolio_type IN ('system','qt_proposal','qt') ORDER BY strategy_id,strategy_name,portfolio_type,symbol FOR SHARE",
                pqxx::params{publication.portfolio_id,publication.source_day});
            for(const auto& row:inventory) {
                if(row[0].as<std::string>()!=publication.strategy_id ||
                    !std::binary_search(expected_owners.begin(),expected_owners.end(),row[1].as<std::string>()) ||
                    row[2].as<std::string>()=="system")
                    throw std::runtime_error("runtime_empty_owner_book_inventory_changed");
            }
            std::vector<QtSeedRow> qt_rows;
            auto physical=txn.exec("SELECT portfolio_id,strategy_id,strategy_name,date::text,symbol,portfolio_type,quantity::text,average_price::text FROM trading.positions WHERE portfolio_id=$1 AND strategy_id=$2 AND date=$3::date AND portfolio_type='qt' ORDER BY portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type",
                pqxx::params{publication.portfolio_id,publication.strategy_id,publication.source_day});
            for(const auto& row:physical)qt_rows.push_back({{row[0].as<std::string>(),row[1].as<std::string>(),row[2].as<std::string>(),row[3].as<std::string>(),row[4].as<std::string>(),row[5].as<std::string>()},exact_position_decimal(row[6].as<std::string>()),exact_position_decimal(row[7].as<std::string>())});
            QtEmptyModelOwnerPublication payload{recorded,pending_publication_->snapshot,expected_owners,pending_publication_->fresh_empty_batches,qt_rows};
            const auto produced=qt_empty_model_owner_document(payload);
            if(produced.is_error())throw std::runtime_error("runtime_empty_owner_document_invalid");
            const auto& owner=produced.value();
            auto captures=txn.exec("SELECT portfolio_config->'config_inspection' FROM trading.live_run_metadata WHERE portfolio_id=$1 AND strategy_id=$2 AND date=$3::date",
                pqxx::params{publication.portfolio_id,publication.strategy_id,publication.source_day});
            if(captures.size()!=1 || captures[0][0].is_null())throw std::runtime_error("runtime_empty_owner_capture_missing");
            const auto capture=nlohmann::json::parse(captures[0][0].as<std::string>());
            const auto& identity=capture.at("identity");
            const auto attempt=pending_publication_->attempt_id.empty()?nlohmann::json(nullptr):nlohmann::json(pending_publication_->attempt_id);
            if(capture.at("publication_schema_version")!=3 || capture.at("profile")!="live_equity_mean_reversion" ||
                capture.at("authority")!="inspection_only" || capture.at("stream")!="system" || capture.at("status")!="available" || capture.at("reason")!="none" ||
                capture.at("equity_run_consumption")!=pending_publication_->equity_final_consumption->document() ||
                capture.at("equity_run_consumption").at("available")!=true || capture.at("equity_run_consumption").at("complete")!=true ||
                expected_owners!=std::vector<std::string>{capture.at("equity_run_consumption").at("run_key").at("strategy_name").get<std::string>()} ||
                identity.at("registry_id")!=pending_publication_->registry_id || identity.at("registry_revision")!=pending_publication_->registry_revision ||
                identity.at("engine_strategy_id")!=publication.strategy_id || identity.at("portfolio_id")!=publication.portfolio_id || identity.at("run_date")!=publication.source_day ||
                identity.at("capture_id")!=publication.publication_id || identity.at("publication_id")!=publication.publication_id || identity.at("runtime_attempt_id")!=attempt ||
                identity.at("producer_version")!=publication.producer_version || identity.at("control_mode")!=(attempt.is_null()?"uncontrolled":"controlled"))
                throw std::runtime_error("runtime_empty_owner_capture_changed");
            nlohmann::json batches=nlohmann::json::array();
            for(const auto& b:pending_publication_->fresh_empty_batches)batches.push_back({{"portfolio_id",b.portfolio_id},{"strategy_id",b.strategy_id},{"strategy_name",b.strategy_name},{"source_day",b.source_day}});
            std::sort(batches.begin(),batches.end(),[](const auto& a,const auto& b){return a.at("strategy_name").template get<std::string>()<b.at("strategy_name").template get<std::string>();});
            const auto inserted_owner=txn.exec("INSERT INTO trading.qt_empty_model_owner_publications(publication_id,attempt_id,schema_version,portfolio_id,strategy_id,source_day,publication_version,registry_id,registry_revision,configured_owner_names,configuration_snapshot,configuration_digest,fresh_empty_batches,inspection_capture,system_components,seed_digest,proposal_components,proposal_manifest_digest,qt_components,qt_digest,producer_version,created_at) VALUES($1::uuid,$2::uuid,$3,$4,$5,$6::date,$7,$8,$9,$10::jsonb,$11::jsonb,$12,$13::jsonb,$14::jsonb,$15::jsonb,$16,$17::jsonb,$18,$19::jsonb,$20,$21,clock_timestamp())",
                pqxx::params{publication.publication_id,pending_publication_->attempt_id.empty()?std::optional<std::string>{}:std::optional<std::string>{pending_publication_->attempt_id},
                    owner.at("schema_version").get<std::string>(),publication.portfolio_id,publication.strategy_id,publication.source_day,version,pending_publication_->registry_id,pending_publication_->registry_revision,
                    owner.at("configured_owner_names").dump(),pending_publication_->snapshot.dump(),owner.at("configuration_digest").get<std::string>(),batches.dump(),capture.dump(),
                    owner.at("system_components").dump(),owner.at("seed_digest").get<std::string>(),owner.at("proposal_components").dump(),owner.at("proposal_manifest_digest").get<std::string>(),
                    owner.at("qt_components").dump(),owner.at("qt_digest").get<std::string>(),publication.producer_version});
            if(inserted_owner.affected_rows()!=1)throw std::runtime_error("runtime_empty_owner_archive_not_inserted");
            return Result<void>();
        }
        txn.exec(
            "INSERT INTO trading.qt_model_seed_publications "
            "(publication_id,attempt_id,portfolio_id,strategy_id,source_day,"
            "publication_version,system_components,seed_digest,proposal_components,"
            "proposal_manifest_digest,producer_version) "
            "VALUES ($1::uuid,$2::uuid,$3,$4,$5::date,$6,$7::jsonb,$8,$9::jsonb,$10,$11)",
            pqxx::params{publication.publication_id,
                pending_publication_->attempt_id.empty()
                    ? std::optional<std::string>{}
                    : std::optional<std::string>{pending_publication_->attempt_id},
                publication.portfolio_id,publication.strategy_id,publication.source_day,
                version,document.value().at("seed_rows").dump(),
                publication.seed_digest,manifest.value().at("proposal_rows").dump(),
                manifest_digest.value(),publication.producer_version});
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "qt_model_seed_record_failed: " + std::string(e.what()));
    }
}

Result<PostgresDatabase::ReportPositionRows> PostgresDatabase::load_qt_proposal_positions_by_date(
    const std::string& strategy_id, const std::vector<std::string>& strategy_names,
    const std::string& portfolio_id, const Timestamp& date) {
    if (!nonblank_proposal_identity(strategy_id) ||
        !nonblank_proposal_identity(portfolio_id) || strategy_names.empty() ||
        std::any_of(strategy_names.begin(),strategy_names.end(),
            [](const auto& name){ return !nonblank_proposal_identity(name); }))
        return make_error<ReportPositionRows>(ErrorCode::INVALID_ARGUMENT,"proposal_scope_invalid");
    auto validation = validate_connection();
    if (validation.is_error())
        return make_error<ReportPositionRows>(validation.error()->code(),validation.error()->what());
    try {
        PublicationTransaction txn(*connection_,publication_transaction_);
        require_proposal_capability(txn);
        std::string quoted_names;
        for (const auto& name : strategy_names) {
            if (!quoted_names.empty()) quoted_names += ",";
            quoted_names += txn.quote(name);
        }
        const auto day = format_timestamp(date).substr(0,10);
        if (!valid_proposal_day(day))
            return make_error<ReportPositionRows>(ErrorCode::INVALID_ARGUMENT,"proposal_day_invalid");
        auto rows = txn.exec(
            "SELECT symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,"
            "EXTRACT(EPOCH FROM last_update),strategy_name FROM trading.positions "
            "WHERE strategy_id=$1 AND portfolio_id=$2 AND date=$3::date "
            "AND portfolio_type='qt_proposal' AND strategy_name IN ("+quoted_names+")",
            pqxx::params{strategy_id,portfolio_id,day});
        ReportPositionRows result;
        for (const auto& row : rows) {
            const auto epoch = std::chrono::duration<double>(row[5].as<double>());
            const auto timestamp = Timestamp(
                std::chrono::duration_cast<Timestamp::duration>(epoch));
            const auto symbol = row[0].as<std::string>();
            result[row[6].as<std::string>()][symbol] = Position(symbol,
                exact_position_decimal(row[1].as<std::string>()),
                exact_position_decimal(row[2].as<std::string>()),
                exact_position_decimal(row[3].as<std::string>()),
                exact_position_decimal(row[4].as<std::string>()),timestamp);
        }
        txn.commit();
        return Result<ReportPositionRows>(std::move(result));
    } catch (const std::exception& e) {
        return make_error<ReportPositionRows>(ErrorCode::DATABASE_ERROR,
            "proposal_reader_failed: " + std::string(e.what()));
    }
}

Result<void> PostgresDatabase::store_trading_equity_curve(const std::string& strategy_id,
                                                          const Timestamp& timestamp, double equity,
                                                          const std::string& portfolio_id,
                                                          const std::string& table_name,
                                                          const std::string& portfolio_type) {
    if (portfolio_type == "qt_proposal") {
        poison_proposal_refusal();
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"proposal_equity_unsupported");
    }
    if (portfolio_type == "qt") {
        auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
        if (stream_validation.is_error()) return stream_validation;
    }
    if (defer_live_write(timestamp,[this,strategy_id,timestamp,equity,portfolio_id,table_name,portfolio_type]() {
        return store_trading_equity_curve(strategy_id,timestamp,equity,portfolio_id,table_name,portfolio_type);
    }, portfolio_type == "system" ? static_cast<unsigned>(EquityPart) : 0u)) return Result<void>();
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);


        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        // The ON CONFLICT target must name the columns of an ACTUAL unique
        // constraint. Migration 001 rebuilds trading_equity_curve_unique to
        // include portfolio_type, so naming the old three columns after that
        // migration fails outright with "no unique or exclusion constraint
        // matching the ON CONFLICT specification". Detecting the column at
        // runtime keeps this correct on both sides of the migration.
        const bool has_portfolio_type = column_exists(txn, table_name, "portfolio_type");
        if (!has_portfolio_type && portfolio_type != "system") {
            return make_error<void>(
                ErrorCode::DATABASE_ERROR,
                "portfolio_type=" + portfolio_type + " requested but " + table_name +
                    " has no portfolio_type column. Apply migrations/001_add_portfolio_type.sql "
                    "before writing a non-system stream.",
                "PostgresDatabase");
        }

        std::string query;
        if (has_portfolio_type) {
            query = "INSERT INTO " + table_name +
                    " (strategy_id, timestamp, equity, portfolio_id, portfolio_type) "
                    "VALUES ($1, $2, $3, $4, $5) "
                    "ON CONFLICT (portfolio_id, strategy_id, timestamp, portfolio_type) "
                    "DO UPDATE SET equity = EXCLUDED.equity";
            txn.exec(query, pqxx::params{strategy_id, format_timestamp(timestamp), equity,
                                         portfolio_id, portfolio_type});
        } else {
            query = "INSERT INTO " + table_name +
                    " (strategy_id, timestamp, equity, portfolio_id) "
                    "VALUES ($1, $2, $3, $4) "
                    "ON CONFLICT (portfolio_id, strategy_id, timestamp) "
                    "DO UPDATE SET equity = EXCLUDED.equity";
            txn.exec(query,
                     pqxx::params{strategy_id, format_timestamp(timestamp), equity, portfolio_id});
        }

        txn.commit();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store trading equity curve: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_equity_trading_equity_curve(
    const std::string& strategy_id,const Timestamp& timestamp,double equity,
    const std::string& portfolio_id) {
    // This additive entry is used only by the explicit equity results manager.
    // Keep the default writer byte-identical for existing futures profiles.
    auto admitted=validate_operational_stream(portfolio_id,"system");
    if(admitted.is_error())return admitted;
    if(defer_live_write(timestamp,[this,strategy_id,timestamp,equity,portfolio_id]() {
        return store_equity_trading_equity_curve(strategy_id,timestamp,equity,portfolio_id);
    },static_cast<unsigned>(EquityPart)))return Result<void>();
    auto validation=validate_connection();if(validation.is_error())return validation;
    try {
        PublicationTransaction txn(*connection_,publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,"system");
        if(!column_exists(txn,"trading.equity_curve","portfolio_type"))
            return make_error<void>(ErrorCode::DATABASE_ERROR,"explicit_equity_curve_requires_stream_schema");
        txn.exec("INSERT INTO trading.equity_curve "
            "(strategy_id,timestamp,equity,portfolio_id,portfolio_type) "
            "VALUES($1,$2::timestamptz,$3,$4,'system') "
            "ON CONFLICT(portfolio_id,strategy_id,timestamp,portfolio_type) "
            "DO UPDATE SET equity=EXCLUDED.equity",
            pqxx::params{strategy_id,format_timestamp(timestamp)+"+00",equity,portfolio_id});
        txn.commit();return Result<void>();
    } catch(const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
            "Failed to store trading equity curve: "+std::string(e.what()),"PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_trading_equity_curve_batch(
    const std::string& strategy_id, const std::vector<std::pair<Timestamp, double>>& equity_points,
    const std::string& portfolio_id, const std::string& table_name, const std::string& portfolio_type) {
    auto stream_validation = validate_operational_stream(portfolio_id,portfolio_type);
    if (stream_validation.is_error()) return stream_validation;
    // A batch can span history and current day. Do not silently split its commit.
    if (pending_publication_)
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"runtime_batch_equity_refused");
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        PublicationTransaction txn(*connection_, publication_transaction_);
        fence_live_write(txn,strategy_id,portfolio_id,portfolio_type);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        for (const auto& [timestamp, equity] : equity_points) {
            std::string query = "INSERT INTO " + table_name +
                                " (strategy_id, timestamp, equity, portfolio_id, portfolio_type) "
                                "VALUES ($1, $2, $3, $4, $5) "
                                "ON CONFLICT (portfolio_id, strategy_id, timestamp, portfolio_type) "
                                "DO UPDATE SET equity = EXCLUDED.equity";

            txn.exec(query, pqxx::params{strategy_id, format_timestamp(timestamp), equity, portfolio_id,portfolio_type});
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(equity_points.size()) +
             " equity curve points for strategy: " + strategy_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store trading equity curve batch: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::convert_generic_to_arrow(
    const pqxx::result& result) const {
    if (result.empty()) {
        // Create an empty table with empty schema
        auto schema = arrow::schema({});
        std::vector<std::shared_ptr<arrow::Array>> empty_arrays;
        auto empty_table = arrow::Table::Make(schema, empty_arrays);
        return Result<std::shared_ptr<arrow::Table>>(empty_table);
    }

    try {
        arrow::MemoryPool* pool = arrow::default_memory_pool();
        std::vector<std::shared_ptr<arrow::Field>> fields;
        std::vector<std::shared_ptr<arrow::Array>> arrays;

        // Iterate through columns to build schema and arrays
        for (pqxx::row::size_type col = 0; col < result.columns(); ++col) {
            std::string col_name = result.column_name(col);

            // Build a string array for all columns (simplest approach)
            // For production, you'd want to detect types properly
            arrow::StringBuilder builder(pool);

            // Reserve space
            if (builder.Reserve(result.size()) != arrow::Status::OK()) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::CONVERSION_ERROR,
                    "Failed to reserve memory for column: " + col_name);
            }

            // Add values
            for (const auto& row : result) {
                if (row[col].is_null()) {
                    if (builder.AppendNull() != arrow::Status::OK()) {
                        return make_error<std::shared_ptr<arrow::Table>>(
                            ErrorCode::CONVERSION_ERROR,
                            "Failed to append null value for column: " + col_name);
                    }
                } else {
                    // Try to get value as string
                    std::string val = row[col].as<std::string>();
                    if (builder.Append(val) != arrow::Status::OK()) {
                        return make_error<std::shared_ptr<arrow::Table>>(
                            ErrorCode::CONVERSION_ERROR,
                            "Failed to append value for column: " + col_name);
                    }
                }
            }

            // Finish array
            std::shared_ptr<arrow::Array> array;
            if (builder.Finish(&array) != arrow::Status::OK()) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::CONVERSION_ERROR, "Failed to finish array for column: " + col_name);
            }

            // Add field and array
            fields.push_back(arrow::field(col_name, arrow::utf8()));
            arrays.push_back(array);
        }

        // Create schema and table
        auto schema = arrow::schema(fields);
        auto table = arrow::Table::Make(schema, arrays);

        return Result<std::shared_ptr<arrow::Table>>(table);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Exception during generic Arrow table conversion: " + std::string(e.what()));
    }
}

Result<std::vector<PostgresDatabase::AppliedCorpActionRow>>
PostgresDatabase::load_applied_corp_actions(const std::string& portfolio_id,
                                            const std::string& strategy_id,
                                            const std::string& strategy_name) {
    using Rows = std::vector<AppliedCorpActionRow>;

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Rows>(validation.error()->code(), validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);
        // Whole-history load: this is the strategy's lifetime dedup set and the
        // source for cumulative dividend income. Parameterised, and the PK
        // (portfolio_id, strategy_id, strategy_name, ...) serves the prefix
        // scan directly. strategy_name must match the write key: one
        // strategy_id spans several names, and dropping it returns another
        // strategy's applied events as if they were this one's.
        pqxx::result result = txn.exec_params(
            "SELECT symbol, action_type, ex_date::text AS ex_date, "
            "COALESCE(qty_held, 0) AS qty_held, "
            "COALESCE(dividend_per_share, 0) AS dividend_per_share, "
            "COALESCE(total_cash, 0) AS total_cash, "
            // E2-F23 / migration 005. Empty string for a legacy row, which the
            // caller reads as "unknown" and accepts -- refusing every row written
            // before the column existed would make the next run unstartable.
            "COALESCE(run_date::text, '') AS run_date, "
            // F-8 / migration 006. NULL means the ratio was never recorded, which
            // is NOT the same as "the event moved no basis" -- the two are
            // separated here rather than collapsed into a 1.0 the caller cannot
            // tell from a real one.
            "basis_ratio IS NOT NULL AS basis_ratio_known, "
            "COALESCE(basis_ratio, 1) AS basis_ratio "
            "FROM trading.corp_action_applied "
            "WHERE portfolio_id = $1 AND strategy_id = $2 AND strategy_name = $3",
            portfolio_id, strategy_id, strategy_name);

        Rows out;
        out.reserve(result.size());
        for (const auto& row : result) {
            AppliedCorpActionRow r;
            r.symbol = row["symbol"].c_str();
            r.action_type = row["action_type"].c_str();
            r.ex_date = row["ex_date"].c_str();
            r.qty_held = row["qty_held"].as<double>();
            r.dividend_per_share = row["dividend_per_share"].as<double>();
            r.total_cash = row["total_cash"].as<double>();
            r.run_date = row["run_date"].c_str();
            r.basis_ratio_known = row["basis_ratio_known"].as<bool>();
            r.basis_ratio = row["basis_ratio"].as<double>();
            out.push_back(std::move(r));
        }

        txn.commit();
        return Result<Rows>(std::move(out));

    } catch (const std::exception& e) {
        return make_error<Rows>(
            ErrorCode::DATABASE_ERROR,
            "Failed to load applied corp actions: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_applied_corp_actions(
    const std::string& portfolio_id, const std::string& strategy_id,
    const std::string& strategy_name,
    const std::vector<AppliedCorpActionRow>& rows) {
    if (rows.empty()) return Result<void>();

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<void>(validation.error()->code(), validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);
        auto stored =
            store_applied_corp_actions_in(txn, portfolio_id, strategy_id, strategy_name, rows);
        if (stored.is_error())
            return stored;
        txn.commit();
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store applied corp actions: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_applied_corp_actions(
    DbTransaction& txn, const std::string& portfolio_id, const std::string& strategy_id,
    const std::string& strategy_name,
    const std::vector<AppliedCorpActionRow>& rows) {
    if (rows.empty()) return Result<void>();
    if (!txn.valid() || txn.committed()) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "store_applied_corp_actions called with a moved-from unit of work",
            "PostgresDatabase");
    }
    return store_applied_corp_actions_in(txn.work(), portfolio_id, strategy_id, strategy_name,
                                         rows);
}

Result<void> PostgresDatabase::store_applied_corp_actions_in(
    pqxx::work& txn, const std::string& portfolio_id, const std::string& strategy_id,
    const std::string& strategy_name,
    const std::vector<AppliedCorpActionRow>& rows) {
    if (rows.empty()) return Result<void>();

    auto admitted=validate_operational_stream(portfolio_id,"system");
    if(admitted.is_error()) return admitted;
    for(const auto& owner:{strategy_id,strategy_name,portfolio_id}) {
        auto valid=validate_strategy_id(owner);if(valid.is_error())return valid;
    }
    try {
        fence_live_write(txn,strategy_id,portfolio_id,"system");
        // DO NOTHING rather than DO UPDATE: the first application is the
        // authoritative one. A repeated run must not rewrite qty_held with a
        // post-adjustment quantity.
        for (const auto& r : rows) {
            // run_date is the writing pass's OWN as-of date (E2-F23, migration
            // 005), not now(): a replay of 2026-04-07 executed tonight must stamp
            // 2026-04-07, or a later chain's rows would look like an earlier
            // chain's and the detector would never fire. Empty stores NULL rather
            // than an epoch date, so an unstamped row stays honestly unknown.
            txn.exec_params(
                "INSERT INTO trading.corp_action_applied "
                "(portfolio_id, strategy_id, strategy_name, symbol, action_type, "
                " ex_date, qty_held, dividend_per_share, total_cash, run_date, "
                " basis_ratio) "
                "VALUES ($1, $2, $3, $4, $5, $6::date, $7, $8, $9, "
                "        NULLIF($10, '')::date, $11) "
                "ON CONFLICT (portfolio_id, strategy_id, strategy_name, symbol, "
                "             action_type, ex_date) DO NOTHING",
                portfolio_id, strategy_id, strategy_name, r.symbol, r.action_type,
                r.ex_date, r.qty_held, r.dividend_per_share, r.total_cash, r.run_date,
                // F-8 / migration 006: NULL when the caller had no ratio to record
                // (a TERMINATION restates nothing), so an absent ratio stays
                // honestly absent rather than becoming an identity factor.
                r.basis_ratio_known ? std::optional<double>(r.basis_ratio)
                                    : std::optional<double>());
        }
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store applied corp actions: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

DbTransaction::DbTransaction(pqxx::connection& conn)
    : txn_(std::make_unique<pqxx::work>(conn)) {}

DbTransaction::DbTransaction(DbTransaction&& other) noexcept
    : txn_(std::move(other.txn_)), committed_(other.committed_) {
    other.committed_ = false;
}

DbTransaction& DbTransaction::operator=(DbTransaction&& other) noexcept {
    if (this != &other) {
        txn_ = std::move(other.txn_);
        committed_ = other.committed_;
        other.committed_ = false;
    }
    return *this;
}

DbTransaction::~DbTransaction() {
    // pqxx::work rolls back on destruction when it was never committed, which is
    // exactly the behaviour we want for an abandoned unit of work. Destroying it
    // here (rather than letting the member die silently) keeps that explicit.
    if (txn_ && !committed_) {
        try {
            txn_->abort();
        } catch (...) {
            // A rollback that itself fails leaves the server to clean up when the
            // connection closes. Nothing useful can be done from a destructor.
        }
    }
}

Result<void> DbTransaction::commit() {
    if (!txn_) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "commit called on a moved-from unit of work", "DbTransaction");
    }
    if (committed_) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "unit of work already committed", "DbTransaction");
    }
    try {
        txn_->commit();
        committed_ = true;
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to commit unit of work: " + std::string(e.what()),
                                "DbTransaction");
    }
}

Result<std::unique_ptr<DbTransaction>> PostgresDatabase::begin_unit_of_work() {
    using Scope = std::unique_ptr<DbTransaction>;

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Scope>(validation.error()->code(), validation.error()->what(),
                                 "PostgresDatabase");
    }
    try {
        // `new` rather than make_unique: the constructor is private to keep
        // pqxx out of caller code, and make_unique is not a friend.
        return Result<Scope>(Scope(new DbTransaction(*connection_)));
    } catch (const std::exception& e) {
        return make_error<Scope>(ErrorCode::DATABASE_ERROR,
                                 "Failed to begin unit of work: " + std::string(e.what()),
                                 "PostgresDatabase");
    }
}

Result<std::vector<PostgresDatabase::TickerAliasRow>>
PostgresDatabase::get_ticker_aliases() {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<TickerAliasRow>>(
            validation.error()->code(), validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);
        auto result = txn.exec(
            "SELECT historical_ticker, current_symbol, effective_until, note "
            "FROM equities_data.ticker_aliases "
            "ORDER BY historical_ticker");

        std::vector<TickerAliasRow> rows;
        rows.reserve(result.size());
        for (const auto& row : result) {
            TickerAliasRow a;
            a.historical_ticker = row["historical_ticker"].is_null()
                                      ? "" : row["historical_ticker"].c_str();
            a.current_symbol = row["current_symbol"].is_null()
                                   ? "" : row["current_symbol"].c_str();
            a.effective_until = row["effective_until"].is_null()
                                    ? "" : row["effective_until"].c_str();
            a.note = row["note"].is_null() ? "" : row["note"].c_str();
            if (a.historical_ticker.empty() || a.current_symbol.empty()) continue;
            rows.push_back(std::move(a));
        }

        txn.commit();
        return Result<std::vector<TickerAliasRow>>(std::move(rows));

    } catch (const std::exception& e) {
        return make_error<std::vector<TickerAliasRow>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch ticker aliases: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::vector<PostgresDatabase::CorpActionRow>>
PostgresDatabase::get_corporate_actions(
    const std::vector<std::string>& tickers,
    const std::string& start_date,
    const std::string& end_date,
    const std::vector<std::string>& actions) {

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<CorpActionRow>>(
            validation.error()->code(), validation.error()->what());
    }
    if (tickers.empty() || actions.empty()) {
        return Result<std::vector<CorpActionRow>>(std::vector<CorpActionRow>{});
    }

    try {
        pqxx::work txn(*connection_);

        // Parameter arrays rather than concatenated IN-lists: at the full
        // 852-symbol universe the string form built a 5 kB literal per call.
        //
        // G6-4: equities_data.corporate_action stores `date` as TEXT, and this
        // compared it as `date::date BETWEEN $3::date AND $4::date`. The cast
        // is evaluated per row and throws on the first value that is not a
        // parseable date, taking the whole query -- and the run -- with it; it
        // also makes the predicate non-sargable, so the (ticker, date) index
        // covers only the ticker side. Every value in the column is a 10-char
        // ISO-8601 date (verified: 0 of 627,169 rows fail
        // '^[0-9]{4}-[0-9]{2}-[0-9]{2}$'), and ISO-8601 sorts lexicographically,
        // so a plain text comparison selects exactly the same rows, index-native
        // and with no cast to fail. The ISO precondition is pinned by
        // tests/live/corp_actions/test_corp_action_query_bounds_db.cpp.
        const std::string query =
            "SELECT date, action, ticker, value, contraticker, contraname, name "
            "FROM equities_data.corporate_action "
            "WHERE ticker = ANY($1) "
            "  AND action = ANY($2) "
            "  AND date >= $3 AND date <= $4 "
            "ORDER BY date, ticker, action";

        auto result = txn.exec(query, pqxx::params{tickers, actions, start_date, end_date});
        std::vector<CorpActionRow> rows;
        rows.reserve(result.size());

        for (const auto& row : result) {
            CorpActionRow ca;
            ca.date_str = row["date"].c_str();
            ca.action = row["action"].c_str();
            ca.ticker = row["ticker"].c_str();
            // value is stored as text in the source schema; parse defensively.
            const std::string val_str = row["value"].is_null() ? "" : row["value"].c_str();
            try {
                ca.value = val_str.empty() ? 0.0 : std::stod(val_str);
            } catch (const std::exception&) {
                // A TERMINATION row legitimately carries no numeric value
                // (a delisting has no ratio); only price-restating rows need
                // one, and those are sourced per-bar now.
                ca.value = 0.0;
            }
            ca.contra_ticker = row["contraticker"].is_null() ? "" : row["contraticker"].c_str();
            ca.contra_name = row["contraname"].is_null() ? "" : row["contraname"].c_str();
            ca.name = row["name"].is_null() ? "" : row["name"].c_str();
            rows.push_back(std::move(ca));
        }

        txn.commit();
        return Result<std::vector<CorpActionRow>>(std::move(rows));

    } catch (const std::exception& e) {
        return make_error<std::vector<CorpActionRow>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch corporate actions: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::string> PostgresDatabase::get_corp_action_feed_last_date(
    const std::string& as_of_date) {

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::string>(validation.error()->code(), validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);
        // Text comparison for the same reason get_corporate_actions uses one:
        // the column is TEXT holding ISO-8601, so max() and the bound are both
        // lexicographic and index-friendly, and no row can throw on a cast.
        pqxx::result r =
            as_of_date.empty()
                ? txn.exec("SELECT COALESCE(max(date), '') FROM equities_data.corporate_action")
                : txn.exec_params(
                      "SELECT COALESCE(max(date), '') FROM equities_data.corporate_action "
                      "WHERE date <= $1",
                      as_of_date);
        std::string last;
        if (!r.empty() && !r[0][0].is_null()) last = r[0][0].c_str();
        txn.commit();
        return Result<std::string>(std::move(last));

    } catch (const std::exception& e) {
        return make_error<std::string>(
            ErrorCode::DATABASE_ERROR,
            "Failed to read the corporate-action feed's last row date: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::vector<PostgresDatabase::CorpActionRow>>
PostgresDatabase::get_per_bar_corporate_actions(
    const std::vector<std::string>& tickers,
    const std::string& start_date,
    const std::string& end_date) {

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<CorpActionRow>>(
            validation.error()->code(), validation.error()->what());
    }
    if (tickers.empty()) {
        return Result<std::vector<CorpActionRow>>(std::vector<CorpActionRow>{});
    }

    try {
        pqxx::work txn(*connection_);

        // div_cash and split_factor are written on the bar the event goes ex
        // and are never restated, so this window is exact. split_factor = 1
        // and div_cash = 0 are the no-event values; NULLs are treated the
        // same way. The vendor also encodes ADR-ratio changes and spin-offs
        // in split_factor, so all three surface here as "split".
        //
        // Half-open UTC timestamp range rather than `time::date BETWEEN`:
        // casting the indexed column makes the predicate non-sargable, so the
        // planner abandoned the (symbol, time) index and seq-scanned the whole
        // 936 MB table -- 4.57 M rows read to return 89, on every live run.
        // Measured at the full 852-symbol universe: 12,342 ms -> 107 ms.
        //
        // The range is [start 00:00 UTC, end+1 00:00 UTC), which selects exactly
        // the same bars the date cast did on a UTC host. That equivalence is the
        // reason this ships with the timezone fix rather than separately: on a
        // TZ=America/New_York host the old cast silently selected a
        // day-shifted set.
        //
        // Symbols bind as a parameter array, matching the adjustment query,
        // instead of a 5 kB quoted IN-list built by string concatenation.
        const std::string query =
            "SELECT (time AT TIME ZONE 'UTC')::date AS ex_date, symbol, "
            "       COALESCE(div_cash, 0) AS div_cash, "
            "       COALESCE(split_factor, 1) AS split_factor "
            "FROM equities_data.ohlcv_1d "
            "WHERE symbol = ANY($1) "
            "  AND time >= ($2::date::timestamp AT TIME ZONE 'UTC') "
            "  AND time < (($3::date + INTERVAL '1 day') AT TIME ZONE 'UTC') "
            "  AND (COALESCE(div_cash, 0) <> 0 "
            "       OR COALESCE(split_factor, 1) NOT IN (0, 1)) "
            "ORDER BY ex_date, symbol";

        auto result = txn.exec(query, pqxx::params{tickers, start_date, end_date});
        std::vector<CorpActionRow> rows;
        rows.reserve(result.size() * 2);

        for (const auto& row : result) {
            const std::string ex_date = row["ex_date"].c_str();
            const std::string symbol = row["symbol"].c_str();
            const double div_cash = row["div_cash"].as<double>(0.0);
            const double split_factor = row["split_factor"].as<double>(1.0);

            // A bar can carry both (e.g. a spin-off dividend alongside a
            // ratio change); emit each as its own event. Splits first: the
            // applier scales quantity before the dividend rescales basis, so
            // the per-share amount lands on the post-split share count.
            if (split_factor != 0.0 && split_factor != 1.0) {
                CorpActionRow ca;
                ca.ticker = symbol;
                ca.date_str = ex_date;
                ca.action = "split";
                ca.value = split_factor;
                rows.push_back(std::move(ca));
            }
            if (div_cash != 0.0) {
                CorpActionRow ca;
                ca.ticker = symbol;
                ca.date_str = ex_date;
                ca.action = "dividend";
                ca.value = div_cash;
                rows.push_back(std::move(ca));
            }
        }

        txn.commit();
        return Result<std::vector<CorpActionRow>>(std::move(rows));

    } catch (const std::exception& e) {
        return make_error<std::vector<CorpActionRow>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch per-bar corporate actions: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, std::string>>
PostgresDatabase::get_delisting_dates(const std::vector<std::string>& tickers,
                                     const std::string& from_date) {
    using Map = std::unordered_map<std::string, std::string>;

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Map>(validation.error()->code(), validation.error()->what());
    }
    if (tickers.empty()) {
        return Result<Map>(Map{});
    }

    try {
        pqxx::work txn(*connection_);

        // Parameter array, matching the other equity readers. The partial index
        // idx_ohlcv_1d_delisting (migration 003) covers the IS NOT NULL
        // predicate, which is what took this from 14.1 s to 1.9 s at 852
        // symbols.
        // BA-8: bound the row by date. Without a floor this returns
        // max(delisting_date) over the symbol's ENTIRE history, so a reused
        // ticker inherits a dead company's delisting (HPC 2008-11-24, MER
        // 2008-12-31) and a held position is exited at a stale price. The
        // runner's bars-contradict guard cannot cover this on its own:
        // delisting_is_stale() is false when last_bar_date is empty, which is
        // exactly the symbol that stopped printing.
        //
        // Compared as text -- delisting_date is a date column and the bound is
        // ISO, which orders lexicographically; cast the bound, not the column,
        // so the partial index idx_ohlcv_1d_delisting still applies.
        pqxx::result result;
        if (from_date.empty()) {
            result = txn.exec(
                "SELECT symbol, max(delisting_date)::text AS delisting_date "
                "FROM equities_data.ohlcv_1d "
                "WHERE symbol = ANY($1) AND delisting_date IS NOT NULL "
                "GROUP BY symbol",
                pqxx::params{tickers});
        } else {
            result = txn.exec(
                "SELECT symbol, max(delisting_date)::text AS delisting_date "
                "FROM equities_data.ohlcv_1d "
                "WHERE symbol = ANY($1) AND delisting_date IS NOT NULL "
                "  AND delisting_date >= $2::date "
                "GROUP BY symbol",
                pqxx::params{tickers, from_date});
        }

        Map out;
        for (const auto& row : result) {
            if (row["delisting_date"].is_null()) continue;
            out.emplace(row["symbol"].c_str(), row["delisting_date"].c_str());
        }

        txn.commit();
        return Result<Map>(std::move(out));

    } catch (const std::exception& e) {
        return make_error<Map>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch delisting dates: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

namespace {
bool equity_model_history_day(const std::string& date) {
    if(date.size()!=10 || date[4]!='-' || date[7]!='-') return false;
    for(size_t i=0;i<date.size();++i)
        if(i!=4 && i!=7 && (date[i]<'0' || date[i]>'9')) return false;
    if(date.substr(0,4)=="0000") return false;
    Timestamp parsed;
    return core::parse_utc_date(date,parsed) && core::format_utc_date(parsed)==date;
}
}

Result<std::unordered_map<std::string, std::string>>
PostgresDatabase::get_position_inception_dates(const std::string& strategy_id,
                                               const std::string& strategy_name,
                                               const std::string& portfolio_id,
                                               const std::vector<std::string>& symbols,
                                               const std::string& on_or_before,
                                               const std::string& table_name) {
    using Map = std::unordered_map<std::string, std::string>;

    if (table_name != "trading.positions") return make_error<Map>(ErrorCode::INVALID_ARGUMENT,
        "equity_model_history_table_invalid");
    if (!equity_model_history_day(on_or_before)) return make_error<Map>(ErrorCode::INVALID_ARGUMENT,
        "equity_model_history_date_invalid");
    for(const auto& owner:{strategy_id,strategy_name,portfolio_id}) {
        auto valid=validate_strategy_id(owner);
        if(valid.is_error()) return make_error<Map>(valid.error()->code(),valid.error()->what());
    }
    auto valid_symbols=validate_symbols(symbols);
    if(valid_symbols.is_error()) return make_error<Map>(valid_symbols.error()->code(),valid_symbols.error()->what());

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Map>(validation.error()->code(), validation.error()->what());
    }
    if (symbols.empty()) return Result<Map>(Map{});

    try {
        pqxx::work txn(*connection_);

        // Earliest date this strategy ever held the symbol non-zero. Wider than
        // the current unbroken holding period when a position was closed and
        // reopened, which is deliberate: over-fetching is rejected by
        // trading.corp_action_applied, while under-fetching corrupts a basis
        // permanently.
        auto result = txn.exec(
            "SELECT symbol, min(date)::text AS inception "
            "FROM " + table_name +
                " WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3 "
                "  AND portfolio_type = 'system' "
                "  AND symbol = ANY($4) AND quantity <> 0 AND date <= $5::date "
                "GROUP BY symbol",
            pqxx::params{strategy_id, strategy_name, portfolio_id, symbols, on_or_before});

        Map out;
        for (const auto& row : result) {
            if (row["inception"].is_null()) continue;
            out.emplace(row["symbol"].c_str(), row["inception"].c_str());
        }

        txn.commit();
        return Result<Map>(std::move(out));

    } catch (const std::exception& e) {
        return make_error<Map>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch position inception dates: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, std::string>>
PostgresDatabase::get_current_holding_start_dates(const std::string& strategy_id,
                                                  const std::string& strategy_name,
                                                  const std::string& portfolio_id,
                                                  const std::vector<std::string>& symbols,
                                                  const std::string& on_or_before,
                                                  const std::string& table_name) {
    using Map = std::unordered_map<std::string, std::string>;

    if (table_name != "trading.positions") return make_error<Map>(ErrorCode::INVALID_ARGUMENT,
        "equity_model_history_table_invalid");
    if (!equity_model_history_day(on_or_before)) return make_error<Map>(ErrorCode::INVALID_ARGUMENT,
        "equity_model_history_date_invalid");
    for(const auto& owner:{strategy_id,strategy_name,portfolio_id}) {
        auto valid=validate_strategy_id(owner);
        if(valid.is_error()) return make_error<Map>(valid.error()->code(),valid.error()->what());
    }
    auto valid_symbols=validate_symbols(symbols);
    if(valid_symbols.is_error()) return make_error<Map>(valid_symbols.error()->code(),valid_symbols.error()->what());

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Map>(validation.error()->code(), validation.error()->what());
    }
    if (symbols.empty()) return Result<Map>(Map{});

    try {
        pqxx::work txn(*connection_);

        // The start of the holding we hold NOW: the earliest non-zero row that is
        // NEWER than the most recent flat row for the same key. A close writes exactly
        // one quantity-0 row on its own date (E2-F19), so that row is the break between
        // a previous holding and this one. No flat row => never closed => this equals
        // min(date), the lifetime inception.
        //
        // Deliberately NOT the same question as get_position_inception_dates. That one
        // fails wide for the class-1 price window; this one is the class-2 rename era
        // and must fail narrow (BA-2).
        //
        // Both scans bind the same historical date and system stream.
        auto result = txn.exec(
            "SELECT symbol, min(date)::text AS holding_start "
            "FROM " + table_name + " p "
                " WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3 "
                "  AND portfolio_type = 'system' "
                "  AND symbol = ANY($4) AND quantity <> 0 "
                "  AND date <= $5::date "
                "  AND date > COALESCE(("
                "        SELECT max(z.date) FROM " + table_name + " z "
                "         WHERE z.strategy_id = p.strategy_id "
                "           AND z.strategy_name = p.strategy_name "
                "           AND z.portfolio_id = p.portfolio_id "
                "           AND z.portfolio_type = 'system' "
                "           AND z.symbol = p.symbol AND z.quantity = 0 "
                "           AND z.date <= $5::date), "
                "      DATE '1900-01-01') "
                "GROUP BY symbol",
            pqxx::params{strategy_id, strategy_name, portfolio_id, symbols,
                         on_or_before});

        Map out;
        for (const auto& row : result) {
            if (row["holding_start"].is_null()) continue;
            out.emplace(row["symbol"].c_str(), row["holding_start"].c_str());
        }

        txn.commit();
        return Result<Map>(std::move(out));

    } catch (const std::exception& e) {
        return make_error<Map>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch current holding start dates: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, std::string>>
PostgresDatabase::get_last_buy_dates(const std::string& strategy_id,
                                     const std::string& strategy_name,
                                     const std::string& portfolio_id,
                                     const std::vector<std::string>& symbols,
                                     const std::string& on_or_after,
                                     const std::string& on_or_before,
                                     const std::string& table_name) {
    using Map = std::unordered_map<std::string, std::string>;

    if (table_name != "trading.executions") return make_error<Map>(ErrorCode::INVALID_ARGUMENT,
        "equity_model_history_table_invalid");
    if (!equity_model_history_day(on_or_before) || !equity_model_history_day(on_or_after) || on_or_after > on_or_before) return make_error<Map>(ErrorCode::INVALID_ARGUMENT,
        "equity_model_history_date_invalid");
    for(const auto& owner:{strategy_id,strategy_name,portfolio_id}) {
        auto valid=validate_strategy_id(owner);
        if(valid.is_error()) return make_error<Map>(valid.error()->code(),valid.error()->what());
    }
    auto valid_symbols=validate_symbols(symbols);
    if(valid_symbols.is_error()) return make_error<Map>(valid_symbols.error()->code(),valid_symbols.error()->what());

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Map>(validation.error()->code(), validation.error()->what());
    }
    if (symbols.empty()) return Result<Map>(Map{});

    try {
        pqxx::work txn(*connection_);

        // BUY only: it is the sole fill that re-forms a long book's weighted cost basis. A SELL
        // reduces quantity and realizes P&L but leaves average_price untouched, so it carries no
        // information about the basis frame.
        auto result = txn.exec(
            "SELECT symbol, max(date)::text AS last_buy "
            "FROM " + table_name +
                " WHERE strategy_id = $1 AND strategy_name = $2 AND portfolio_id = $3 "
                "  AND portfolio_type = 'system' "
                "  AND symbol = ANY($4) AND side = 'BUY' "
                "  AND date >= $5 AND date <= $6 "
                "GROUP BY symbol",
            pqxx::params{strategy_id, strategy_name, portfolio_id, symbols,
                         on_or_after, on_or_before});

        Map out;
        for (const auto& row : result) {
            if (row["last_buy"].is_null()) continue;
            out.emplace(row["symbol"].c_str(), row["last_buy"].c_str());
        }

        txn.commit();
        return Result<Map>(std::move(out));

    } catch (const std::exception& e) {
        return make_error<Map>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch last BUY dates: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, std::map<std::string, double>>>
PostgresDatabase::get_historical_closes(const std::vector<std::string>& symbols,
                                        const std::string& start_date,
                                        const std::string& end_date) {
    using Map = std::unordered_map<std::string, std::map<std::string, double>>;

    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Map>(validation.error()->code(), validation.error()->what());
    }
    if (symbols.empty()) return Result<Map>(Map{});

    try {
        pqxx::work txn(*connection_);

        // Half-open timestamp range so the (symbol, time) primary key is usable;
        // a time::date cast here would be non-sargable, which is what made the
        // per-bar event query 14.3 s before migration 003.
        auto result = txn.exec(
            "SELECT symbol, (time AT TIME ZONE 'UTC')::date::text AS bar_date, close "
            "FROM equities_data.ohlcv_1d "
            "WHERE symbol = ANY($1) AND time >= ($2::date::timestamp AT TIME ZONE 'UTC') "
            "AND time < (($3::date + 1)::timestamp AT TIME ZONE 'UTC') "
            "  AND close IS NOT NULL "
            "ORDER BY symbol, time",
            pqxx::params{symbols, start_date, end_date});

        Map out;
        for (const auto& row : result) {
            out[row["symbol"].c_str()][row["bar_date"].c_str()] =
                row["close"].as<double>();
        }

        txn.commit();
        return Result<Map>(std::move(out));

    } catch (const std::exception& e) {
        return make_error<Map>(
            ErrorCode::DATABASE_ERROR,
            "Failed to fetch historical closes: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_positions(DbTransaction& scope,
    const std::vector<Position>& positions, const std::string& strategy_id,
    const std::string& strategy_name, const std::string& portfolio_id,
    const std::string& table_name) {
    if (!scope.valid() || scope.committed()) return make_error<void>(ErrorCode::DATABASE_ERROR,
        "system_equity_scope_not_mutable");
    // Only historical SystemReference equity corrections may commit separately.
    // Current/future outputs and VerifiedDeskPrior never enter this old system-ledger path.
    if(publication_transaction_ || (pending_publication_ &&
        (pending_publication_->invalid_payload || positions.empty() ||
         pending_publication_->strategy_id!="LIVE_EQUITY_MEAN_REVERSION" ||
         strategy_id!=pending_publication_->strategy_id || strategy_name!="EQUITY_MEAN_REVERSION" ||
         portfolio_id!=pending_publication_->portfolio_id ||
         pending_publication_->prior_requirement!=PublicationPriorRequirement::None ||
         core::format_utc_date(positions.front().last_update)>=pending_publication_->date)))
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,"equity_uow_cannot_bypass_active_publication");
    if (table_name != "trading.positions") return make_error<void>(ErrorCode::INVALID_ARGUMENT,
        "equity_uow_requires_trading_positions");
    for (const auto& id : {strategy_id,strategy_name,portfolio_id}) {
        auto valid=validate_strategy_id(id);if(valid.is_error())return valid;
    }
    auto admitted=validate_operational_stream(portfolio_id,"system");
    if(admitted.is_error()) return admitted;
    if(positions.empty()) return Result<void>();
    const auto day=core::format_utc_date(positions.front().last_update);
    for(const auto& position:positions) {
        auto valid=validate_position(position);if(valid.is_error())return valid;
        if(core::format_utc_date(position.last_update)!=day)
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,"equity_uow_mixed_position_dates");
    }
    try {
        auto& txn=scope.work();
        fence_live_write(txn,strategy_id,portfolio_id,"system");
        // Values are bound, and every delete includes book, owner, date and stream.
        // No fallback can expand the affected owner scope on an SQL error.
        txn.exec_params("DELETE FROM trading.positions WHERE portfolio_id=$1 AND strategy_id=$2 "
            "AND strategy_name=$3 AND date=$4::date AND portfolio_type='system'",
            portfolio_id,strategy_id,strategy_name,day);
        for(const auto& position:positions) {
            const auto stamp=core::format_utc_datetime(position.last_update)+"+00";
            txn.exec_params("INSERT INTO trading.positions "
                "(symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,updated_at,"
                "strategy_id,strategy_name,date,portfolio_id,portfolio_type) "
                "VALUES ($1,$2,$3,$4,$5,$6::timestamptz,$6::timestamptz,$7,$8,$9::date,$10,'system')",
                position.symbol,position.quantity.as_double(),position.average_price.as_double(),
                position.unrealized_pnl.as_double(),position.realized_pnl.as_double(),stamp,
                strategy_id,strategy_name,day,portfolio_id);
        }
        return Result<void>();
    } catch(const std::exception& error) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
            "Failed to store equity positions in unit of work: "+std::string(error.what()));
    }
}

}  // namespace trade_ngin

namespace trade_ngin {
Result<std::unordered_map<std::string,Position>> PostgresDatabase::load_equity_model_system_positions(
    const std::string& portfolio_id,const Timestamp& date) {
    return load_equity_system_positions_by_owner(
        "LIVE_EQUITY_MEAN_REVERSION", "EQUITY_MEAN_REVERSION", portfolio_id, date);
}

Result<std::unordered_map<std::string,Position>>
PostgresDatabase::load_equity_system_positions_by_owner(
    const std::string& strategy_id, const std::string& strategy_name,
    const std::string& portfolio_id, const Timestamp& date) {try {
    if(portfolio_id.empty() || portfolio_id.size()>100 || strategy_id.empty() ||
       strategy_name.empty())throw std::invalid_argument("equity_system_owner_invalid");
    for(const auto& id:{strategy_id,strategy_name,portfolio_id}) {
        auto id_valid=validate_strategy_id(id);
        if(id_valid.is_error())throw std::invalid_argument("equity_system_owner_invalid");
    }
    auto stream_valid=validate_operational_stream(portfolio_id,"system");
    if(stream_valid.is_error())throw std::invalid_argument("equity_system_owner_invalid");
    auto valid=validate_connection();if(valid.is_error())return make_error<std::unordered_map<std::string,Position>>(valid.error()->code(),valid.error()->what());
    PublicationTransaction tx(*connection_,publication_transaction_);
    const auto rows=tx.exec("SELECT symbol,quantity::text,average_price::text,daily_unrealized_pnl::text,daily_realized_pnl::text,"
        "to_char(last_update AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"') "
        "FROM trading.positions WHERE strategy_id=$1 AND strategy_name=$2 "
        "AND portfolio_id=$3 AND portfolio_type='system' AND date=$4::date ORDER BY symbol",
        pqxx::params{strategy_id,strategy_name,portfolio_id,core::format_utc_date(date)});
    if(rows.size()>4096)throw std::invalid_argument("equity_system_capacity");
    std::unordered_map<std::string,Position> result;
    for(const auto& row:rows){for(std::size_t i=0;i<6;++i)if(row[i].is_null())throw std::invalid_argument("equity_system_null");
        const auto symbol=row[0].as<std::string>();if(symbol.empty() || result.contains(symbol))throw std::invalid_argument("equity_system_duplicate");
        const auto stamp=row[5].as<std::string>();Timestamp marked;
        if(stamp.size()!=27 || stamp[10]!='T' || stamp[19]!='.' || stamp[26]!='Z')throw std::invalid_argument("equity_system_timestamp");
        Timestamp midnight;if(!core::parse_utc_date(stamp.substr(0,10),midnight))throw std::invalid_argument("equity_system_timestamp");
        const int hours=std::stoi(stamp.substr(11,2)),minutes=std::stoi(stamp.substr(14,2)),seconds=std::stoi(stamp.substr(17,2));
        if(hours>23 || minutes>59 || seconds>59)throw std::invalid_argument("equity_system_timestamp");
        marked=midnight+std::chrono::hours(hours)+std::chrono::minutes(minutes)+std::chrono::seconds(seconds)+std::chrono::microseconds(std::stoi(stamp.substr(20,6)));
        result.emplace(symbol,Position{symbol,exact_position_decimal(row[1].as<std::string>()),
            exact_position_decimal(row[2].as<std::string>()),exact_position_decimal(row[3].as<std::string>()),
            exact_position_decimal(row[4].as<std::string>()),marked});
    }
    tx.commit();return result;
}catch(const std::exception&){return make_error<std::unordered_map<std::string,Position>>(ErrorCode::INVALID_DATA,"equity_system_prior_unavailable");}}
}
