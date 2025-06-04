// src/data/postgres_database.cpp

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include <sstream>
#include <iomanip>

namespace trade_ngin {

PostgresDatabase::PostgresDatabase(std::string connection_string)
    : connection_string_(std::move(connection_string)),
      connection_(nullptr) {
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
            return make_error<void>(
                ErrorCode::CONNECTION_ERROR,
                "Failed to open database connection",
                "PostgresDatabase"
            );
        }

        // Generate a unique ID for this connection instance
        static std::atomic<int> counter{0};
        std::string unique_id = "POSTGRES_DB_" + std::to_string(++counter);

        // Register with state manager using the unique ID
        ComponentInfo info{
            ComponentType::MARKET_DATA,
            ComponentState::INITIALIZED,
            unique_id,
            "",
            std::chrono::system_clock::now(),
            {}
        };

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
        return make_error<void>(
            ErrorCode::CONNECTION_ERROR,
            "Database connection error: " + std::string(e.what()),
            "PostgresDatabase"
        );
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
    const std::vector<std::string>& symbols,
    const Timestamp& start_date,
    const Timestamp& end_date,
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type) {
        if (start_date > end_date) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::INVALID_ARGUMENT,
                "Start date must be before end date"
            );
        }
    
        auto validation = validate_connection();
        if (validation.is_error()) {
            return make_error<std::shared_ptr<arrow::Table>>(
                validation.error()->code(),
                validation.error()->what()
            );
        }

        try {
            std::string query = build_market_data_query(
                symbols, start_date, end_date, asset_class, freq, data_type);

            pqxx::work txn(*connection_);
            auto result = txn.exec(query);
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
                ErrorCode::DATABASE_ERROR,
                "Failed to fetch market data: " + std::string(e.what()),
                "PostgresDatabase"
            );
        }
}

Result<void> PostgresDatabase::store_executions(
    const std::vector<ExecutionReport>& executions,
    const std::string& table_name) {
    
    auto validation = validate_connection();
    if (validation.is_error()) return validation;

    try {
        pqxx::work txn(*connection_);
        
        for (const auto& exec : executions) {
            txn.exec_params(
                "INSERT INTO " + table_name + 
                " (order_id, exec_id, symbol, side, quantity, price, "
                "execution_time, commission, is_partial) VALUES "
                "($1, $2, $3, $4, $5, $6, $7, $8, $9)",
                exec.order_id,
                exec.exec_id,
                exec.symbol,
                side_to_string(exec.side),
                exec.filled_quantity,
                exec.fill_price,
                format_timestamp(exec.fill_time),
                exec.commission,
                exec.is_partial
            );
        }

        txn.commit();
        return Result<void>();
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store executions: " + std::string(e.what()),
            "PostgresDatabase"
        );
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
            return make_error<void>(
                ErrorCode::CONNECTION_ERROR,  
                "Not connected to database",
                "PostgresDatabase"
            );
        }
    return Result<void>();
}

Result<void> PostgresDatabase::store_positions(
    const std::vector<Position>& positions,
    const std::string& table_name) {
    
    auto validation = validate_connection();
    if (validation.is_error()) return validation;

    try {
        pqxx::work txn(*connection_);
        
        // Begin transaction
        txn.exec("BEGIN");
        
        // Clear existing positions
        txn.exec("DELETE FROM " + table_name);
        
        // Insert new positions
        for (const auto& pos : positions) {
            txn.exec_params(
                "INSERT INTO " + table_name + 
                " (symbol, quantity, average_price, unrealized_pnl, "
                "realized_pnl, last_update) VALUES "
                "($1, $2, $3, $4, $5, $6)",
                pos.symbol,
                pos.quantity,
                pos.average_price,
                pos.unrealized_pnl,
                pos.realized_pnl,
                format_timestamp(pos.last_update)
            );
        }

        txn.commit();
        INFO("Successfully updated " + std::to_string(positions.size()) + " positions");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store positions: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

Result<void> PostgresDatabase::store_signals(
    const std::unordered_map<std::string, double>& signals,
    const std::string& strategy_id,
    const Timestamp& timestamp,
    const std::string& table_name) {
    
    auto validation = validate_connection();
    if (validation.is_error()) return validation;

    try {
        pqxx::work txn(*connection_);
        
        for (const auto& [symbol, signal] : signals) {
            txn.exec_params(
                "INSERT INTO " + table_name + 
                " (strategy_id, symbol, signal_value, timestamp) VALUES "
                "($1, $2, $3, $4) "
                "ON CONFLICT (strategy_id, symbol, timestamp) "
                "DO UPDATE SET signal_value = EXCLUDED.signal_value",
                strategy_id,
                symbol,
                signal,
                format_timestamp(timestamp)
            );
        }

        txn.commit();
        INFO("Successfully stored signals for strategy: " + strategy_id);
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Failed to store signals: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

Result<std::vector<std::string>> PostgresDatabase::get_symbols(
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type) {
    
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::vector<std::string>>(
            validation.error()->code(),
            validation.error()->what()
        );
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        pqxx::work txn(*connection_);
        
        auto result = txn.exec(
            "WITH latest_data AS ("
            "   SELECT DISTINCT ON (symbol) symbol, time "
            "   FROM " + full_table_name + " "
            "   ORDER BY symbol, time DESC"
            ") "
            "SELECT symbol "
            "FROM latest_data "
            "ORDER BY symbol"
        );
        
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
            ErrorCode::DATABASE_ERROR,
            "Failed to get symbols: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::execute_query(
    const std::string& query) {
    
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            validation.error()->code(),
            validation.error()->what()
        );
    }

    try {
        pqxx::work txn(*connection_);
        auto result = txn.exec(query);
        txn.commit();
        return convert_to_arrow_table(result);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::DATABASE_ERROR,
            "Failed to execute query: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::get_contract_metadata() const {
    
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            validation.error()->code(),
            validation.error()->what(),
            "PostgresDatabase"
        );
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
            "Failed to retrieve contract metadata: " + std::string(e.what()),
            "PostgresDatabase"
        );
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

std::string PostgresDatabase::build_market_data_query(
    const std::vector<std::string>& symbols,
    const Timestamp& start_date,
    const Timestamp& end_date,
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type) const {
    
    std::string full_table_name = build_table_name(asset_class, data_type, freq);
    
    std::ostringstream query;
    query << "SELECT time, symbol, open, high, low, close, volume "
          << "FROM " << full_table_name
          << " WHERE time BETWEEN '" << format_timestamp(start_date)
          << "' AND '" << format_timestamp(end_date) << "'";

    if (!symbols.empty()) {
        query << " AND symbol IN (";
        for (size_t i = 0; i < symbols.size(); ++i) {
            if (i > 0) query << ",";
            query << "'" << pqxx::to_string(symbols[i]) << "'";
        }
        query << ")";
    }

    query << " ORDER BY symbol, time";
    return query.str();
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::convert_to_arrow_table(
    const pqxx::result& result) const {
    
    if (result.empty()) {
        // Return empty table with schema
        auto schema = arrow::schema({
            arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
            arrow::field("symbol", arrow::utf8()),
            arrow::field("open", arrow::float64()),
            arrow::field("high", arrow::float64()),
            arrow::field("low", arrow::float64()),
            arrow::field("close", arrow::float64()),
            arrow::field("volume", arrow::float64())
        });

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
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR, 
            "Arrow builder error during " + operation
        );
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
            
            auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                tp.time_since_epoch()).count();

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
        std::shared_ptr<arrow::Array> timestamp_array, symbol_array, 
            open_array, high_array, low_array, close_array, volume_array;

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
        auto schema = arrow::schema({
            arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
            arrow::field("symbol", arrow::utf8()),
            arrow::field("open", arrow::float64()),
            arrow::field("high", arrow::float64()),
            arrow::field("low", arrow::float64()),
            arrow::field("close", arrow::float64()),
            arrow::field("volume", arrow::float64())
        });

        // Create and return table
        auto table = arrow::Table::Make(schema, {
            timestamp_array,
            symbol_array,
            open_array,
            high_array,
            low_array,
            close_array,
            volume_array
        });

        return Result<std::shared_ptr<arrow::Table>>(table);

    } catch (const std::exception& e) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Exception during Arrow table conversion: " + std::string(e.what())
        );
    }
}

Result<std::shared_ptr<arrow::Table>> PostgresDatabase::convert_metadata_to_arrow(
    const pqxx::result& result) const {    
    if (result.empty()) {
        // Return empty table with schema
        auto schema = arrow::schema({
            arrow::field("Name", arrow::utf8()),
            arrow::field("Databento Symbol", arrow::utf8()),
            arrow::field("IB Symbol", arrow::utf8()),
            arrow::field("Asset Type", arrow::utf8()),
            arrow::field("Sector", arrow::utf8()),
            arrow::field("Exchange", arrow::utf8()),
            arrow::field("Contract Size", arrow::float64()),
            arrow::field("Minimum Price Fluctuation", arrow::float64()),
            arrow::field("Tick Size", arrow::utf8()),
            arrow::field("Trading Hours (EST)", arrow::utf8()),
            arrow::field("Overnight Initial Margin", arrow::float64()),
            arrow::field("Overnight Maintenance Margin", arrow::float64()),
            arrow::field("Intraday Initial Margin", arrow::float64()),
            arrow::field("Intraday Maintenance Margin", arrow::float64()),
            arrow::field("Units", arrow::utf8()),
            arrow::field("Data Provider", arrow::utf8()),
            arrow::field("Dataset", arrow::utf8()),
            arrow::field("Contract Months", arrow::utf8())
        });

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
    for (auto* builder : {&name_builder, &databento_symbol_builder, &ib_symbol_builder, 
                         &asset_type_builder, &sector_builder, &exchange_builder,
                         &tick_size_builder, &trading_hours_builder, &units_builder,
                         &data_provider_builder, &dataset_builder, &contract_months_builder}) {
        auto status = builder->Reserve(result.size());
        if (!status.ok()) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to reserve memory for string builder: " + status.ToString(),
                "PostgresDatabase"
            );
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
                "PostgresDatabase"
            );
        }
    }

    // Define column indices based on debug output
    // Indexes from the debug log: "Databento Symbol, IB Symbol, Name, Exchange, Intraday Initial Margin, ..."
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
    const int SETTLEMENT_TYPE_IDX = 14; // Not needed in final table
    const int TRADING_HOURS_IDX = 15;
    const int DATA_PROVIDER_IDX = 16;
    const int DATASET_IDX = 17;
    const int NEWEST_MONTH_ADDITIONS_IDX = 18; // Not needed in final table
    const int CONTRACT_MONTHS_IDX = 19;
    const int TIME_OF_EXPIRY_IDX = 20; // Not needed in final table

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
                            clean_str.erase(std::remove(clean_str.begin(), clean_str.end(), ','), clean_str.end());
                            clean_str.erase(std::remove(clean_str.begin(), clean_str.end(), '%'), clean_str.end());
                            
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
            (void)append_double(overnight_initial_margin_builder, row, OVERNIGHT_INITIAL_MARGIN_IDX);
            (void)append_double(overnight_maintenance_margin_builder, row, OVERNIGHT_MAINTENANCE_MARGIN_IDX);
            (void)append_double(intraday_initial_margin_builder, row, INTRADAY_INITIAL_MARGIN_IDX);
            (void)append_double(intraday_maintenance_margin_builder, row, INTRADAY_MAINTENANCE_MARGIN_IDX);
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
    std::shared_ptr<arrow::Array> name_array, databento_symbol_array, ib_symbol_array, asset_type_array,
        sector_array, exchange_array, contract_size_array, min_tick_array, tick_size_array,
        trading_hours_array, overnight_initial_margin_array, overnight_maintenance_margin_array, intraday_initial_margin_array,
        intraday_maintenance_margin_array, units_array, data_provider_array, dataset_array, contract_months_array;
    
    arrow::Status status;
    
    // Finish each builder and capture any errors
    status = name_builder.Finish(&name_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Name' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = databento_symbol_builder.Finish(&databento_symbol_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Databento Symbol' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = ib_symbol_builder.Finish(&ib_symbol_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'IB Symbol' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = asset_type_builder.Finish(&asset_type_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Asset Type' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = sector_builder.Finish(&sector_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Sector' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = exchange_builder.Finish(&exchange_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Exchange' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = contract_size_builder.Finish(&contract_size_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Contract Size' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = min_tick_builder.Finish(&min_tick_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Minimum Price Fluctuation' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = tick_size_builder.Finish(&tick_size_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Tick Size' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = trading_hours_builder.Finish(&trading_hours_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Trading Hours' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = overnight_initial_margin_builder.Finish(&overnight_initial_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Overnight Initial Margin' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = overnight_maintenance_margin_builder.Finish(&overnight_maintenance_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Overnight Maintenance Margin' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = intraday_initial_margin_builder.Finish(&intraday_initial_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Intraday Initial Margin' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = intraday_maintenance_margin_builder.Finish(&intraday_maintenance_margin_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Intraday Maintenance Margin' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = units_builder.Finish(&units_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Units' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = data_provider_builder.Finish(&data_provider_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Data Provider' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = dataset_builder.Finish(&dataset_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Dataset' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }
    
    status = contract_months_builder.Finish(&contract_months_array);
    if (!status.ok()) {
        return make_error<std::shared_ptr<arrow::Table>>(
            ErrorCode::CONVERSION_ERROR,
            "Failed to finish 'Contract Months' array: " + status.ToString(),
            "PostgresDatabase"
        );
    }

    // Create schema
    auto schema = arrow::schema({
        arrow::field("Name", arrow::utf8()),
        arrow::field("Databento Symbol", arrow::utf8()),
        arrow::field("IB Symbol", arrow::utf8()),
        arrow::field("Asset Type", arrow::utf8()),
        arrow::field("Sector", arrow::utf8()),
        arrow::field("Exchange", arrow::utf8()),
        arrow::field("Contract Size", arrow::float64()),
        arrow::field("Minimum Price Fluctuation", arrow::float64()),
        arrow::field("Tick Size", arrow::utf8()),
        arrow::field("Trading Hours (EST)", arrow::utf8()),
        arrow::field("Overnight Initial Margin", arrow::float64()),
        arrow::field("Overnight Maintenance Margin", arrow::float64()),
        arrow::field("Intraday Initial Margin", arrow::float64()),
        arrow::field("Intraday Maintenance Margin", arrow::float64()),
        arrow::field("Units", arrow::utf8()),
        arrow::field("Data Provider", arrow::utf8()),
        arrow::field("Dataset", arrow::utf8()),
        arrow::field("Contract Months", arrow::utf8())
    });

    // Create and return table
    auto table = arrow::Table::Make(schema, {
        name_array,
        databento_symbol_array,
        ib_symbol_array,
        asset_type_array,
        sector_array,
        exchange_array,
        contract_size_array,
        min_tick_array,
        tick_size_array,
        trading_hours_array,
        overnight_initial_margin_array,
        overnight_maintenance_margin_array,
        intraday_initial_margin_array,
        intraday_maintenance_margin_array,
        units_array,
        data_provider_array,
        dataset_array,
        contract_months_array
    });

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

Result<Timestamp> PostgresDatabase::get_latest_data_time(
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type) const {
    
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<Timestamp>(
            validation.error()->code(),
            validation.error()->what()
        );
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        pqxx::work txn(*connection_);
        
        auto result = txn.exec1(
            "SELECT MAX(time) FROM " + full_table_name
        );
        
        if (result[0].is_null()) {
            return make_error<Timestamp>(
                ErrorCode::DATA_NOT_FOUND,
                "No data found in " + full_table_name
            );
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
        return make_error<Timestamp>(
            ErrorCode::DATABASE_ERROR,
            "Failed to get latest data time: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

Result<std::pair<Timestamp, Timestamp>> PostgresDatabase::get_data_time_range(
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& data_type) const {
    
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<std::pair<Timestamp, Timestamp>>(
            validation.error()->code(),
            validation.error()->what()
        );
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        pqxx::work txn(*connection_);
        
        auto result = txn.exec1(
            "SELECT MIN(time), MAX(time) FROM " + full_table_name
        );
        
        if (result[0].is_null() || result[1].is_null()) {
            return make_error<std::pair<Timestamp, Timestamp>>(
                ErrorCode::DATA_NOT_FOUND,
                "No data found in " + full_table_name
            );
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
            ErrorCode::DATABASE_ERROR,
            "Failed to get data time range: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

Result<size_t> PostgresDatabase::get_data_count(
    AssetClass asset_class,
    DataFrequency freq,
    const std::string& symbol,
    const std::string& data_type) const {
    
    auto validation = validate_connection();
    if (validation.is_error()) {
        return make_error<size_t>(
            validation.error()->code(),
            validation.error()->what()
        );
    }

    try {
        std::string full_table_name = build_table_name(asset_class, data_type, freq);
        pqxx::work txn(*connection_);
        
        auto result = txn.exec1(
            "SELECT COUNT(*) FROM " + full_table_name +
            " WHERE symbol = " + txn.quote(symbol)
        );
        
        return Result<size_t>(result[0].as<size_t>());

    } catch (const std::exception& e) {
        return make_error<size_t>(
            ErrorCode::DATABASE_ERROR,
            "Failed to get data count: " + std::string(e.what()),
            "PostgresDatabase"
        );
    }
}

} // namespace trade_ngin