// include/trade_ngin/data/postgres_database.hpp

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <optional>
#include <pqxx/pqxx>
#include <arrow/api.h>
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

/**
 * @brief Database interface for PostgreSQL
 */
class PostgresDatabase : public DatabaseInterface {
public:
    /**
     * @brief Constructor
     * @param connection_string Connection string for PostgreSQL
     */
    explicit PostgresDatabase(std::string connection_string);

    /**
     * @brief Destructor
     */
    ~PostgresDatabase() override;

    // Delete copy and move operations
    PostgresDatabase(const PostgresDatabase&) = delete;
    PostgresDatabase& operator=(const PostgresDatabase&) = delete;
    PostgresDatabase(PostgresDatabase&&) = delete;
    PostgresDatabase& operator=(PostgresDatabase&&) = delete;

    /**
     * @brief Connect to the database
     * @return Result indicating success or failure
     */
    Result<void> connect() override;

    /**
     * @brief Disconnect from the database
     */
    void disconnect() override;

    /**
     * @brief Check if the database connection is active
     * @return True if connected, false otherwise
     */
    bool is_connected() const override;

    /**
     * @brief Get market data for a list of symbols
     * @param symbols List of symbols to retrieve
     * @param start_date Start date for data retrieval
     * @param end_date End date for data retrieval
     * @param asset_class Asset class for the data
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the market data
     */
    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override;

    /**
     * @brief Store execution reports in the database
     * @param executions List of execution reports
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_executions(
        const std::vector<ExecutionReport>& executions,
        const std::string& table_name) override;

    /**
     * @brief Store positions in the database
     * @param positions List of positions
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_positions(
        const std::vector<Position>& positions,
        const std::string& table_name) override;

    /**
     * @brief Store signals in the database
     * @param signals Map of signals by symbol
     * @param strategy_id ID of the strategy generating the signals
     * @param timestamp Timestamp for the signals
     * @param table_name Name of the table to store data
     * @return Result indicating success or failure
     */
    Result<void> store_signals(
        const std::unordered_map<std::string, double>& signals,
        const std::string& strategy_id,
        const Timestamp& timestamp,
        const std::string& table_name) override;

    /**
     * @brief Get a list of symbols from the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the list of symbols
     */
    Result<std::vector<std::string>> get_symbols(
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override;

    /**
     * @brief Execute a query and return the result as an Arrow table
     * @param query SQL query to execute
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> execute_query(
        const std::string& query) override;

    /**
     * @brief Get contract metadata for trading instruments
     * @return Result containing Arrow table with contract metadata
     */
    Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const;

    /**
     * @brief Convert asset class to string for database queries
     * @param asset_class Asset class to convert
     * @return String representation for database queries
     */
    std::string asset_class_to_string(AssetClass asset_class) const;
    
    /**
     * @brief Get the connection string
     * @return Connection string
     */
    std::string get_connection_string() const { return connection_string_; }
    
    /**
     * @brief Get the component ID
     * @return Component ID
     */
    const std::string& get_component_id() const { return component_id_; }

private:
    std::string connection_string_;
    std::unique_ptr<pqxx::connection> connection_;
    std::mutex mutex_;
    std::string component_id_;

    /**
     * @brief Validate the database connection
     * @return Result indicating success or failure
     */
    Result<void> validate_connection() const;

    /**
     * @brief Format a timestamp as a string
     * @param ts Timestamp to format
     * @return Formatted string
     */
    std::string format_timestamp(const Timestamp& ts) const;

    /**
     * @brief Convert a Side enum to a string
     * @param side Side to convert
     * @return String representation of the side
     */
    std::string side_to_string(Side side) const;

    /**
     * @brief Build a query to retrieve market data
     * @param symbols List of symbols to retrieve
     * @param start_date Start date for data retrieval
     * @param end_date End date for data retrieval
     * @param asset_class Asset class for the data
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return SQL query string
     */
    std::string build_market_data_query(
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& data_type) const;

    /**
     * @brief Convert a pqxx result to an Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> convert_to_arrow_table(
        const pqxx::result& result) const;

    /**
     * @brief Convert contract metadata result to Arrow table
     * @param result pqxx result to convert
     * @return Result containing the Arrow table
     */
    Result<std::shared_ptr<arrow::Table>> convert_metadata_to_arrow(
        const pqxx::result& result) const;

    /**
     * @brief Get the latest data time for a given asset class and frequency
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the latest data time
     */
    Result<Timestamp> get_latest_data_time(
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& data_type = "ohlcv") const;

    /**
     * @brief Get the time range for data in the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param data_type Type of data to retrieve
     * @return Result containing the time range
     */
    Result<std::pair<Timestamp, Timestamp>> get_data_time_range(
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& data_type = "ohlcv") const;

    /**
     * @brief Get the number of data points in the database
     * @param asset_class Asset class to retrieve
     * @param freq Data frequency
     * @param symbol Symbol to retrieve
     * @param data_type Type of data to retrieve
     * @return Result containing the number of data points
     */
    Result<size_t> get_data_count(
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& symbol,
        const std::string& data_type = "ohlcv") const;
};

} // namespace trade_ngin