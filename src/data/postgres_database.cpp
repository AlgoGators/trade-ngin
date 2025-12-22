// src/data/postgres_database.cpp

#include "trade_ngin/data/postgres_database.hpp"
#include <iomanip>
#include <sstream>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"

namespace {
std::string join(const std::vector<std::string>& elements, const std::string& delimiter) {
    std::ostringstream os;
    if (!elements.empty()) {
        os << elements[0];
        for (size_t i = 1; i < elements.size(); ++i) {
            os << delimiter << elements[i];
        }
    }
    return os.str();
}
}
#include "trade_ngin/data/market_data_bus.hpp"

namespace trade_ngin {

PostgresDatabase::PostgresDatabase(std::string connection_string)
    : connection_string_(std::move(connection_string)), connection_(nullptr) {
    Logger::register_component("PostgresDatabase");
}

PostgresDatabase::~PostgresDatabase() {
    disconnect();
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
        pqxx::work txn(*connection_);
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
                                                const std::string& table_name) {
    std::cout << "DEBUG: store_executions called with " << executions.size() << " executions" << std::endl;
    
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
            std::cout << "DEBUG: Table validation failed: " << table_validation.error()->what() << std::endl;
            return table_validation;
        }
        std::cout << "DEBUG: Table validation passed" << std::endl;

        // Defensive cleanup BEFORE starting the insert transaction to avoid nested transactions
        if (!executions.empty()) {
            std::vector<std::string> order_ids;
            order_ids.reserve(executions.size());
            for (const auto& e : executions) {
                order_ids.push_back(e.order_id);
            }
            // Deduplicate order_ids
            std::sort(order_ids.begin(), order_ids.end());
            order_ids.erase(std::unique(order_ids.begin(), order_ids.end()), order_ids.end());

            // Use the date from the first execution's fill_time
            Timestamp date_for_delete = executions.front().fill_time;
            auto del_result = delete_stale_executions(order_ids, date_for_delete, table_name);
            if (del_result.is_error()) {
                std::cout << "DEBUG: Pre-insert delete_stale_executions failed: "
                          << del_result.error()->what() << std::endl;
                return del_result;
            }
        }

        pqxx::work txn(*connection_);

        for (const auto& exec : executions) {
            std::cout << "DEBUG: Processing execution for symbol: " << exec.symbol << std::endl;
            
            // Validate execution data
            std::cout << "DEBUG: About to validate execution report" << std::endl;
            auto exec_validation = validate_execution_report(exec);
            if (exec_validation.is_error()) {
                std::cout << "DEBUG: Execution validation failed: " << exec_validation.error()->what() << std::endl;
                return exec_validation;
            }
            std::cout << "DEBUG: Execution validation passed" << std::endl;

            // FIXED: Added exec_id to the INSERT statement
            std::string query = "INSERT INTO " + table_name +
                                " (exec_id, order_id, symbol, side, quantity, price, "
                                "execution_time, commission, is_partial) VALUES "
                                "($1, $2, $3, $4, $5, $6, $7, $8, $9)";

            std::cout << "DEBUG: About to execute SQL query" << std::endl;
            std::cout << "DEBUG: Query: " << query << std::endl;

            // FIXED: Added exec.exec_id as the first parameter
            txn.exec_params(query, exec.exec_id, exec.order_id, exec.symbol,
                            side_to_string(exec.side), static_cast<double>(exec.filled_quantity),
                            static_cast<double>(exec.fill_price), format_timestamp(exec.fill_time),
                            static_cast<double>(exec.commission), exec.is_partial);
            
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

std::string PostgresDatabase::format_timestamp(const Timestamp& ts) const {
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

Result<void> PostgresDatabase::store_positions(const std::vector<Position>& positions,
                                               const std::string& strategy_id,
                                               const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        // Begin transaction
        txn.exec("BEGIN");

        // Clear existing positions for this strategy and the date of the positions being inserted
        try {
            // Get the date from the first position being inserted (all positions should be from the same date)
            if (!positions.empty()) {
                auto time_t = std::chrono::system_clock::to_time_t(positions[0].last_update);
                std::stringstream ss;
                ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
                std::string position_date = ss.str();
                
                std::string delete_query = "DELETE FROM " + table_name + 
                    " WHERE strategy_id = '" + strategy_id + "' AND DATE(last_update) = '" + position_date + "'";
                txn.exec(delete_query);
            }
        } catch (const std::exception& e) {
            // If strategy_id column doesn't exist, clear all positions for the position date only
            WARN("strategy_id column may not exist, clearing all positions for position date: " + std::string(e.what()));
            
            if (!positions.empty()) {
                auto time_t = std::chrono::system_clock::to_time_t(positions[0].last_update);
                std::stringstream ss;
                ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d");
                std::string position_date = ss.str();
                
                std::string delete_query = "DELETE FROM " + table_name + " WHERE DATE(last_update) = '" + position_date + "'";
                txn.exec(delete_query);
            }
        }

        // Insert new positions using direct SQL like backtest does
        std::vector<std::string> position_values;
        for (const auto& pos : positions) {
            // Debug: Show position values before validation
            DEBUG("Position before validation: " + pos.symbol + 
                  " qty=" + std::to_string(static_cast<double>(pos.quantity)) +
                  " avg_price=" + std::to_string(static_cast<double>(pos.average_price)) +
                  " unrealized=" + std::to_string(static_cast<double>(pos.unrealized_pnl)) +
                  " realized=" + std::to_string(static_cast<double>(pos.realized_pnl)));
            
            // Validate position data
            auto pos_validation = validate_position(pos);
            if (pos_validation.is_error()) {
                ERROR("Position validation failed for " + pos.symbol + ": " + pos_validation.error()->what());
                return pos_validation;
            }

            // Build position value string matching the trading.positions table schema
            // Schema: symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at, strategy_id
            // Use stringstream with high precision to avoid rounding issues
            std::stringstream ss;
            ss << std::setprecision(17);  // Double precision
            ss << "('" << pos.symbol << "', "
               << static_cast<double>(pos.quantity) << ", "
               << static_cast<double>(pos.average_price) << ", "
               << static_cast<double>(pos.unrealized_pnl) << ", "
               << static_cast<double>(pos.realized_pnl) << ", "
               << "'" << format_timestamp(pos.last_update) << "', "
               << "'" << format_timestamp(pos.last_update) << "', "  // updated_at
               << "'" << strategy_id << "')";

            position_values.push_back(ss.str());
        }

        if (!position_values.empty()) {
            // Try with strategy_id column first
            try {
                std::string query = "INSERT INTO " + table_name +
                                    " (symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at, strategy_id) VALUES " +
                                    join(position_values, ", ");

                DEBUG("Executing position insert query: " + query);
                txn.exec(query);
            } catch (const std::exception& e) {
                // If strategy_id column doesn't exist, try without it
                WARN("strategy_id column may not exist, trying without it: " + std::string(e.what()));
                
                // Rebuild position values without strategy_id
                std::vector<std::string> position_values_no_strategy;
                for (const auto& pos : positions) {
                    // Use stringstream with high precision to avoid rounding issues
                    std::stringstream ss;
                    ss << std::setprecision(17);  // Double precision
                    ss << "('" << pos.symbol << "', "
                       << static_cast<double>(pos.quantity) << ", "
                       << static_cast<double>(pos.average_price) << ", "
                       << static_cast<double>(pos.unrealized_pnl) << ", "
                       << static_cast<double>(pos.realized_pnl) << ", "
                       << "'" << format_timestamp(pos.last_update) << "', "
                       << "'" << format_timestamp(pos.last_update) << "')";
                    position_values_no_strategy.push_back(ss.str());
                }

                std::string query = "INSERT INTO " + table_name +
                                    " (symbol, quantity, average_price, unrealized_pnl, realized_pnl, last_update, updated_at) VALUES " +
                                    join(position_values_no_strategy, ", ");

                DEBUG("Executing position insert query without strategy_id: " + query);
                txn.exec(query);
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
                                             const Timestamp& timestamp,
                                             const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

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

            std::string query = "INSERT INTO " + table_name +
                                " (strategy_id, symbol, signal_value, timestamp) VALUES "
                                "($1, $2, $3, $4) "
                                "ON CONFLICT (strategy_id, symbol, timestamp) "
                                "DO UPDATE SET signal_value = EXCLUDED.signal_value";

            txn.exec_params(query, strategy_id, symbol, signal, format_timestamp(timestamp));
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
        pqxx::work txn(*connection_);

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
    const std::vector<std::string>& symbols, AssetClass asset_class,
    DataFrequency freq, const std::string& data_type) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::unordered_map<std::string, double>>(validation.error()->code(),
                                                                   validation.error()->what());
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        pqxx::work txn(*connection_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<std::unordered_map<std::string, double>>(table_validation.error()->code(),
                                                                       table_validation.error()->what());
        }

        // Validate symbols
        auto symbol_validation = validate_symbols(symbols);
        if (symbol_validation.is_error()) {
            return make_error<std::unordered_map<std::string, double>>(symbol_validation.error()->code(),
                                                                       symbol_validation.error()->what());
        }

        // Query to get latest close price for each symbol
        std::string query = 
            "SELECT DISTINCT ON (symbol) symbol, close "
            "FROM " + full_table_name + " "
            "WHERE symbol = ANY($1) "
            "ORDER BY symbol, time DESC";

        auto result = txn.exec_params(query, symbols);
        txn.commit();

        std::unordered_map<std::string, double> prices;
        for (const auto& row : result) {
            std::string symbol = row[0].as<std::string>();
            double price = row[1].as<double>();
            prices[symbol] = price;
        }

        DEBUG("Retrieved latest prices for " + std::to_string(prices.size()) + " symbols from " + full_table_name);
        return Result<std::unordered_map<std::string, double>>(prices);

    } catch (const std::exception& e) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::DATABASE_ERROR, "Failed to get latest prices: " + std::string(e.what()),
            "PostgresDatabase");
    }
}

Result<std::unordered_map<std::string, Position>> PostgresDatabase::load_positions_by_date(
    const std::string& strategy_id, const Timestamp& date,
    const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::unordered_map<std::string, Position>>(validation.error()->code(),
                                                                     validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);
        
        // Query to get positions for a specific strategy and date
        std::string query =
            "SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl, last_update "
            "FROM " + table_name + " "
            "WHERE strategy_id = $1 AND DATE(last_update) = DATE($2)";

        std::string date_str = format_timestamp(date);
        DEBUG("Querying positions for strategy_id: " + strategy_id + ", date: " + date_str);
        DEBUG("Full query: " + query);
        auto result = txn.exec_params(query, strategy_id, date_str);
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
                WARN("Exception parsing timestamp: " + std::string(e.what()) + ", using current time");
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

        DEBUG("Loaded " + std::to_string(positions.size()) + " positions for strategy " + strategy_id + " on " + date_str);
        return Result<std::unordered_map<std::string, Position>>(positions);

    } catch (const std::exception& e) {
        return make_error<std::unordered_map<std::string, Position>>(
            ErrorCode::DATABASE_ERROR, "Failed to load positions by date: " + std::string(e.what()),
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
        pqxx::work txn(*connection_);
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
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<void>(validation.error()->code(), validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);
        txn.exec(query);
        txn.commit();
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR, "Failed to execute direct query: " + std::string(e.what()),
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
        pqxx::work txn(*connection_);
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
    pqxx::work& txn) const {
    // Validate table name components to prevent injection
    auto table_validation = validate_table_name_components(asset_class, data_type, freq);
    if (table_validation.is_error()) {
        return make_error<pqxx::result>(table_validation.error()->code(),
                                        table_validation.error()->what());
    }

    std::string full_table_name = build_table_name(asset_class, data_type, freq);

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
            return Result<pqxx::result>(txn.exec_params(query, start_ts, end_ts));
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
            return Result<pqxx::result>(txn.exec_params(query, start_ts, end_ts, symbols));
        } catch (const std::exception& e) {
            return make_error<pqxx::result>(ErrorCode::DATABASE_ERROR,
                                            "Query execution failed: " + std::string(e.what()));
        }
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::convert_to_arrow_table(
    const pqxx::result& result) const {
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

        std::vector<std::shared_ptr<arrow::Array>> empty_arrays(18);
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
    const int SETTLEMENT_TYPE_IDX = 14;  // Not needed in final table
    const int TRADING_HOURS_IDX = 15;
    const int DATA_PROVIDER_IDX = 16;
    const int DATASET_IDX = 17;
    const int NEWEST_MONTH_ADDITIONS_IDX = 18;  // Not needed in final table
    const int CONTRACT_MONTHS_IDX = 19;
    const int TIME_OF_EXPIRY_IDX = 20;  // Not needed in final table

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
        pqxx::work txn(*connection_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<Timestamp>(table_validation.error()->code(),
                                         table_validation.error()->what());
        }

        std::string query = "SELECT MAX(time) FROM " + full_table_name;
        auto result = txn.exec1(query);

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
        pqxx::work txn(*connection_);

        // Validate table name components
        auto table_validation = validate_table_name_components(asset_class, data_type, freq);
        if (table_validation.is_error()) {
            return make_error<std::pair<Timestamp, Timestamp>>(table_validation.error()->code(),
                                                               table_validation.error()->what());
        }

        std::string query = "SELECT MIN(time), MAX(time) FROM " + full_table_name;
        auto result = txn.exec1(query);

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
        pqxx::work txn(*connection_);

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
        auto result = txn.exec_params1(query, symbol);

        return Result<size_t>(result[0].as<size_t>());

    } catch (const std::exception& e) {
        return make_error<size_t>(ErrorCode::DATABASE_ERROR,
                                  "Failed to get data count: " + std::string(e.what()),
                                  "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::validate_table_name_components(AssetClass asset_class,
                                                              const std::string& data_type,
                                                              DataFrequency freq) const {
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

    std::vector<std::string> forbidden = {"drop",   "delete", "insert", "update", "alter",
                                          "create", "union",  "select", "script",
                                          "--",     "/*",     "*/"};

    for (const auto& forbidden_word : forbidden) {
        if (lower_name.find(forbidden_word) != std::string::npos) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "Invalid table_name: contains forbidden SQL keywords",
                                    "PostgresDatabase");
        }
    }

    return Result<void>();
}

Result<void> PostgresDatabase::validate_symbol(const std::string& symbol) const {
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

Result<void> PostgresDatabase::validate_symbols(const std::vector<std::string>& symbols) const {
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

    if (exec.commission.is_negative() || static_cast<double>(exec.commission) > 1e12) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Invalid commission: must be between 0 and 1e12",
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

Result<void> PostgresDatabase::store_backtest_executions(const std::vector<ExecutionReport>& executions,
                                                         const std::string& run_id,
                                                         const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        // Use batch insert for better performance with large execution sets
        if (executions.size() > 100) {
            // Build a single multi-value INSERT for large batches
            std::string query = "INSERT INTO " + table_name +
                                " (run_id, execution_id, order_id, timestamp, symbol, side, quantity, price, commission, is_partial) VALUES ";
            
            std::vector<std::string> value_strings;
            value_strings.reserve(executions.size());
            
            for (const auto& exec : executions) {
                std::string values = "('" + run_id + "', '" + exec.exec_id + "', '" + exec.order_id + "', '" + 
                                   format_timestamp(exec.fill_time) + "', '" + exec.symbol + "', '" + 
                                   side_to_string(exec.side) + "', " + std::to_string(static_cast<double>(exec.filled_quantity)) + 
                                   ", " + std::to_string(static_cast<double>(exec.fill_price)) + ", " + 
                                   std::to_string(static_cast<double>(exec.commission)) + ", " + 
                                   (exec.is_partial ? "true" : "false") + ")";
                value_strings.push_back(values);
            }
            
            query += pqxx::separated_list(",", value_strings.begin(), value_strings.end());
            txn.exec(query);
        } else {
            // Use parameterized queries for smaller batches
            for (const auto& exec : executions) {
                std::string query = "INSERT INTO " + table_name +
                                    " (run_id, execution_id, order_id, timestamp, symbol, side, quantity, price, commission, is_partial) "
                                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)";

                txn.exec_params(query, run_id, exec.exec_id, exec.order_id, format_timestamp(exec.fill_time),
                                exec.symbol, side_to_string(exec.side), static_cast<double>(exec.filled_quantity),
                                static_cast<double>(exec.fill_price), static_cast<double>(exec.commission), exec.is_partial);
            }
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(executions.size()) + " backtest executions for run: " + run_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest executions: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_executions_with_strategy(
    const std::vector<ExecutionReport>& executions,
    const std::string& run_id,
    const std::string& strategy_id,
    const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        // Use batch insert for better performance with large execution sets
        if (executions.size() > 100) {
            // Build a single multi-value INSERT for large batches with strategy_id
            std::string query = "INSERT INTO " + table_name +
                                " (run_id, strategy_id, execution_id, order_id, timestamp, symbol, side, quantity, price, commission, is_partial) VALUES ";
            
            std::vector<std::string> value_strings;
            value_strings.reserve(executions.size());
            
            for (const auto& exec : executions) {
                std::string values = "('" + run_id + "', '" + strategy_id + "', '" + exec.exec_id + "', '" + exec.order_id + "', '" + 
                                   format_timestamp(exec.fill_time) + "', '" + exec.symbol + "', '" + 
                                   side_to_string(exec.side) + "', " + std::to_string(static_cast<double>(exec.filled_quantity)) + 
                                   ", " + std::to_string(static_cast<double>(exec.fill_price)) + ", " + 
                                   std::to_string(static_cast<double>(exec.commission)) + ", " + 
                                   (exec.is_partial ? "true" : "false") + ")";
                value_strings.push_back(values);
            }
            
            query += pqxx::separated_list(",", value_strings.begin(), value_strings.end());
            txn.exec(query);
        } else {
            // Use parameterized queries for smaller batches with strategy_id
            for (const auto& exec : executions) {
                std::string query = "INSERT INTO " + table_name +
                                    " (run_id, strategy_id, execution_id, order_id, timestamp, symbol, side, quantity, price, commission, is_partial) "
                                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11)";

                txn.exec_params(query, run_id, strategy_id, exec.exec_id, exec.order_id, format_timestamp(exec.fill_time),
                                exec.symbol, side_to_string(exec.side), static_cast<double>(exec.filled_quantity),
                                static_cast<double>(exec.fill_price), static_cast<double>(exec.commission), exec.is_partial);
            }
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(executions.size()) + " backtest executions for run: " + run_id + ", strategy_id: " + strategy_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest executions with strategy: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_signals(const std::unordered_map<std::string, double>& signals,
                                                      const std::string& strategy_id, const std::string& run_id,
                                                      const Timestamp& timestamp,
                                                      const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        for (const auto& [symbol, signal_value] : signals) {
            // For backtest.signals, include portfolio_run_id if run_id looks like a portfolio run_id (contains '&')
            // Schema has portfolio_run_id column (nullable)
            std::string query;
            if (table_name == "backtest.signals" && run_id.find('&') != std::string::npos) {
                // Portfolio run: run_id is portfolio_run_id format, use it for portfolio_run_id column
                query = "INSERT INTO " + table_name +
                        " (run_id, strategy_id, symbol, signal_value, timestamp, portfolio_run_id) "
                        "VALUES ($1, $2, $3, $4, $5, $6) "
                        "ON CONFLICT (run_id, strategy_id, symbol, timestamp) "
                        "DO UPDATE SET signal_value = EXCLUDED.signal_value, portfolio_run_id = EXCLUDED.portfolio_run_id";
                txn.exec_params(query, run_id, strategy_id, symbol, signal_value, format_timestamp(timestamp), run_id);
            } else {
                // Single strategy run or other table: no portfolio_run_id
                query = "INSERT INTO " + table_name +
                        " (run_id, strategy_id, symbol, signal_value, timestamp) "
                        "VALUES ($1, $2, $3, $4, $5) "
                        "ON CONFLICT (run_id, strategy_id, symbol, timestamp) "
                        "DO UPDATE SET signal_value = EXCLUDED.signal_value";
                txn.exec_params(query, run_id, strategy_id, symbol, signal_value, format_timestamp(timestamp));
            }
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(signals.size()) + " backtest signals for strategy: " + strategy_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest signals: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_backtest_metadata(const std::string& run_id, const std::string& name,
                                                       const std::string& description, const Timestamp& start_date,
                                                       const Timestamp& end_date, const nlohmann::json& hyperparameters,
                                                       const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        std::string query = "INSERT INTO " + table_name +
                            " (run_id, name, description, start_date, end_date, hyperparameters) "
                            "VALUES ($1, $2, $3, $4, $5, $6) "
                            "ON CONFLICT (run_id) "
                            "DO UPDATE SET name = EXCLUDED.name, description = EXCLUDED.description, "
                            "start_date = EXCLUDED.start_date, end_date = EXCLUDED.end_date, "
                            "hyperparameters = EXCLUDED.hyperparameters";

        txn.exec_params(query, run_id, name, description, format_timestamp(start_date), format_timestamp(end_date),
                        hyperparameters.dump());

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
    const std::string& run_id,
    const std::string& portfolio_run_id,
    const std::string& strategy_id,
    double strategy_allocation,
    const nlohmann::json& portfolio_config,
    const std::string& name,
    const std::string& description,
    const Timestamp& start_date,
    const Timestamp& end_date,
    const nlohmann::json& hyperparameters,
    const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        std::string query = "INSERT INTO " + table_name +
                            " (run_id, portfolio_run_id, strategy_id, strategy_allocation, portfolio_config, name, description, start_date, end_date, hyperparameters) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10) "
                            "ON CONFLICT (run_id, strategy_id) "
                            "DO UPDATE SET portfolio_run_id = EXCLUDED.portfolio_run_id, "
                            "strategy_allocation = EXCLUDED.strategy_allocation, "
                            "portfolio_config = EXCLUDED.portfolio_config, "
                            "name = EXCLUDED.name, description = EXCLUDED.description, "
                            "start_date = EXCLUDED.start_date, end_date = EXCLUDED.end_date, "
                            "hyperparameters = EXCLUDED.hyperparameters";

        txn.exec_params(query, run_id, portfolio_run_id, strategy_id, strategy_allocation, portfolio_config.dump(),
                        name, description, format_timestamp(start_date), format_timestamp(end_date),
                        hyperparameters.dump());

        txn.commit();
        INFO("Successfully stored backtest metadata with portfolio for run: " + run_id + ", portfolio_run_id: " + portfolio_run_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store backtest metadata with portfolio: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

// ============================================================================
// LIVE TRADING DATA STORAGE IMPLEMENTATIONS
// ============================================================================

Result<void> PostgresDatabase::store_trading_results(const std::string& strategy_id, const Timestamp& date,
                                                     double total_return, double sharpe_ratio, double sortino_ratio,
                                                     double max_drawdown, double calmar_ratio, double volatility,
                                                     int total_trades, double win_rate, double profit_factor,
                                                     double avg_win, double avg_loss, double max_win, double max_loss,
                                                     double avg_holding_period, double var_95, double cvar_95,
                                                     double beta, double correlation, double downside_volatility,
                                                     const nlohmann::json& config,
                                                     const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        std::string query = "INSERT INTO " + table_name +
                            " (strategy_id, date, total_return, sharpe_ratio, sortino_ratio, max_drawdown, "
                            "calmar_ratio, volatility, total_trades, win_rate, profit_factor, avg_win, avg_loss, "
                            "max_win, max_loss, avg_holding_period, var_95, cvar_95, beta, correlation, "
                            "downside_volatility, config) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22) "
                            "ON CONFLICT (strategy_id, date) "
                            "DO UPDATE SET total_return = EXCLUDED.total_return, sharpe_ratio = EXCLUDED.sharpe_ratio, "
                            "sortino_ratio = EXCLUDED.sortino_ratio, max_drawdown = EXCLUDED.max_drawdown, "
                            "calmar_ratio = EXCLUDED.calmar_ratio, volatility = EXCLUDED.volatility, "
                            "total_trades = EXCLUDED.total_trades, win_rate = EXCLUDED.win_rate, "
                            "profit_factor = EXCLUDED.profit_factor, avg_win = EXCLUDED.avg_win, "
                            "avg_loss = EXCLUDED.avg_loss, max_win = EXCLUDED.max_win, max_loss = EXCLUDED.max_loss, "
                            "avg_holding_period = EXCLUDED.avg_holding_period, var_95 = EXCLUDED.var_95, "
                            "cvar_95 = EXCLUDED.cvar_95, beta = EXCLUDED.beta, correlation = EXCLUDED.correlation, "
                            "downside_volatility = EXCLUDED.downside_volatility, config = EXCLUDED.config";

        txn.exec_params(query, strategy_id, format_timestamp(date), total_return, sharpe_ratio, sortino_ratio,
                        max_drawdown, calmar_ratio, volatility, total_trades, win_rate, profit_factor,
                        avg_win, avg_loss, max_win, max_loss, avg_holding_period, var_95, cvar_95,
                        beta, correlation, downside_volatility, config.dump());

        txn.commit();
        INFO("Successfully stored trading results for strategy: " + strategy_id + " on " + format_timestamp(date));
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store trading results: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_live_results(const std::string& strategy_id, const Timestamp& date,
                                                 double total_return, double volatility, double total_pnl,
                                                 double unrealized_pnl, double realized_pnl, double current_portfolio_value,
                                                 double daily_realized_pnl, double daily_unrealized_pnl,
                                                 double portfolio_var, double gross_leverage, double net_leverage,
                                                 double portfolio_leverage, double margin_leverage, double margin_cushion, double max_correlation, double jump_risk,
                                                 double risk_scale, double gross_notional, double net_notional,
                                                 int active_positions, double total_commissions,
                                                 double margin_posted, double cash_available,
                                                 const nlohmann::json& config,
                                                 const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        std::string query = "INSERT INTO " + table_name +
                            " (strategy_id, date, total_return, volatility, total_pnl, total_unrealized_pnl, total_realized_pnl, "
                            "current_portfolio_value, daily_realized_pnl, daily_unrealized_pnl, portfolio_var, "
                            "gross_leverage, net_leverage, portfolio_leverage, margin_leverage, margin_cushion, max_correlation, jump_risk, "
                            "risk_scale, gross_notional, net_notional, active_positions, total_commissions, margin_posted, cash_available, config) "
                            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14, $15, $16, $17, $18, $19, $20, $21, $22, $23, $24, $25, $26) "
                            "ON CONFLICT (strategy_id, date) "
                            "DO UPDATE SET total_return = EXCLUDED.total_return, volatility = EXCLUDED.volatility, "
                            "total_pnl = EXCLUDED.total_pnl, total_unrealized_pnl = EXCLUDED.total_unrealized_pnl, "
                            "total_realized_pnl = EXCLUDED.total_realized_pnl, current_portfolio_value = EXCLUDED.current_portfolio_value, "
                            "daily_realized_pnl = EXCLUDED.daily_realized_pnl, daily_unrealized_pnl = EXCLUDED.daily_unrealized_pnl, "
                            "portfolio_var = EXCLUDED.portfolio_var, gross_leverage = EXCLUDED.gross_leverage, "
                            "net_leverage = EXCLUDED.net_leverage, portfolio_leverage = EXCLUDED.portfolio_leverage, margin_leverage = EXCLUDED.margin_leverage, margin_cushion = EXCLUDED.margin_cushion, "
                            "max_correlation = EXCLUDED.max_correlation, jump_risk = EXCLUDED.jump_risk, "
                            "risk_scale = EXCLUDED.risk_scale, gross_notional = EXCLUDED.gross_notional, "
                            "net_notional = EXCLUDED.net_notional, active_positions = EXCLUDED.active_positions, "
                            "total_commissions = EXCLUDED.total_commissions, margin_posted = EXCLUDED.margin_posted, cash_available = EXCLUDED.cash_available, config = EXCLUDED.config";

        txn.exec_params(query, strategy_id, format_timestamp(date), total_return, volatility, total_pnl,
                        unrealized_pnl, realized_pnl, current_portfolio_value, daily_realized_pnl, daily_unrealized_pnl,
                        portfolio_var, gross_leverage, net_leverage, portfolio_leverage, margin_leverage, margin_cushion, max_correlation, jump_risk,
                        risk_scale, gross_notional, net_notional, active_positions, total_commissions, margin_posted, cash_available, config.dump());

        txn.commit();
        INFO("Successfully stored live results for strategy: " + strategy_id + " on " + format_timestamp(date));
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store live results: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<std::tuple<double, double, double>> PostgresDatabase::get_previous_live_aggregates(
    const std::string& strategy_id, const Timestamp& date, const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::tuple<double, double, double>>(validation.error()->code(),
                                                              validation.error()->what());
    }

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return make_error<std::tuple<double, double, double>>(table_validation.error()->code(),
                                                                  table_validation.error()->what());
        }

        // Build query for the previous calendar day (DATE(date) = DATE($2) - INTERVAL '1 day')
        std::string query =
            "SELECT current_portfolio_value, total_pnl, total_commissions "
            "FROM " + table_name +
            " WHERE strategy_id = $1 AND DATE(date) = (DATE($2) - INTERVAL '1 day') "
            "ORDER BY created_at DESC LIMIT 1";

        auto result = txn.exec_params(query, strategy_id, format_timestamp(date));
        txn.commit();

        double prev_value = 0.0;
        double prev_total_pnl = 0.0;
        double prev_total_commissions = 0.0;

        if (!result.empty()) {
            const auto& row = result[0];
            if (!row[0].is_null()) prev_value = row[0].as<double>();
            if (!row[1].is_null()) prev_total_pnl = row[1].as<double>();
            if (!row[2].is_null()) prev_total_commissions = row[2].as<double>();
        }

        return Result<std::tuple<double, double, double>>(std::make_tuple(prev_value, prev_total_pnl, prev_total_commissions));
    } catch (const std::exception& e) {
        return make_error<std::tuple<double, double, double>>(ErrorCode::DATABASE_ERROR,
                                                              "Failed to fetch previous live aggregates: " + std::string(e.what()),
                                                              "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_trading_equity_curve(const std::string& strategy_id, const Timestamp& timestamp,
                                                          double equity,
                                                          const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        std::string query = "INSERT INTO " + table_name +
                            " (strategy_id, timestamp, equity) "
                            "VALUES ($1, $2, $3) "
                            "ON CONFLICT (strategy_id, timestamp) "
                            "DO UPDATE SET equity = EXCLUDED.equity";

        txn.exec_params(query, strategy_id, format_timestamp(timestamp), equity);

        txn.commit();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to store trading equity curve: " + std::string(e.what()),
                                "PostgresDatabase");
    }
}

Result<void> PostgresDatabase::store_trading_equity_curve_batch(const std::string& strategy_id,
                                                                const std::vector<std::pair<Timestamp, double>>& equity_points,
                                                                const std::string& table_name) {
    auto validation = validate_connection();
    if (validation.is_error())
        return validation;

    try {
        pqxx::work txn(*connection_);

        // Validate table name
        auto table_validation = validate_table_name(table_name);
        if (table_validation.is_error()) {
            return table_validation;
        }

        for (const auto& [timestamp, equity] : equity_points) {
            std::string query = "INSERT INTO " + table_name +
                                " (strategy_id, timestamp, equity) "
                                "VALUES ($1, $2, $3) "
                                "ON CONFLICT (strategy_id, timestamp) "
                                "DO UPDATE SET equity = EXCLUDED.equity";

            txn.exec_params(query, strategy_id, format_timestamp(timestamp), equity);
        }

        txn.commit();
        INFO("Successfully stored " + std::to_string(equity_points.size()) + " equity curve points for strategy: " + strategy_id);
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
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
                    ErrorCode::CONVERSION_ERROR,
                    "Failed to finish array for column: " + col_name);
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

}  // namespace trade_ngin