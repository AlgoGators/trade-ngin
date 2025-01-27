// include/trade_ngin/data/database_interface.hpp

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <arrow/api.h>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"

namespace trade_ngin {

/**
 * @brief Abstract interface for database operations
 * Defines the contract that any database implementation must fulfill
 */
class DatabaseInterface {
public:
    virtual ~DatabaseInterface() = default;

    /**
     * @brief Connect to the database
     * @return Result indicating success or failure
     */
    virtual Result<void> connect() = 0;

    /**
     * @brief Disconnect from the database
     */
    virtual void disconnect() = 0;

    /**
     * @brief Check if connected to the database
     * @return True if connected
     */
    virtual bool is_connected() const = 0;

    /**
     * @brief Get market data for specified symbols and date range
     * @param symbols List of symbols to fetch
     * @param start_date Start date for data
     * @param end_date End date for data
     * @param table_name Name of the table to query
     * @return Result containing Arrow table with market data
     */
    virtual Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& table_name = "ohlcv"
    ) = 0;

    /**
     * @brief Store trade execution data
     * @param executions Vector of execution reports
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_executions(
        const std::vector<ExecutionReport>& executions,
        const std::string& table_name = "trading.executions"
    ) = 0;

    /**
     * @brief Store position data
     * @param positions Vector of positions
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_positions(
        const std::vector<Position>& positions,
        const std::string& table_name = "trading.positions"
    ) = 0;

    /**
     * @brief Store strategy signals
     * @param signals Map of symbol to signal value
     * @param strategy_id ID of the strategy generating signals
     * @param timestamp Timestamp of signals
     * @param table_name Name of the table to insert into
     * @return Result indicating success or failure
     */
    virtual Result<void> store_signals(
        const std::unordered_map<std::string, double>& signals,
        const std::string& strategy_id,
        const Timestamp& timestamp,
        const std::string& table_name = "trading.signals"
    ) = 0;

    /**
     * @brief Get list of available symbols
     * @param table_name Name of the table to query
     * @return Result containing vector of symbols
     */
    virtual Result<std::vector<std::string>> get_symbols(
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& table_name = "ohlcv"
    ) = 0;

    /**
     * @brief Execute a custom SQL query
     * @param query SQL query to execute
     * @return Result containing Arrow table with query results
     */
    virtual Result<std::shared_ptr<arrow::Table>> execute_query(
        const std::string& query
    ) = 0;

protected:
    /**
     * @brief Helper method to validate date range
     * @param start_date Start date
     * @param end_date End date
     * @return Result indicating if date range is valid
     */
    Result<void> validate_date_range(
        const Timestamp& start_date, 
        const Timestamp& end_date
    ) {
        if (end_date < start_date) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "End date must be after start date",
                "DatabaseInterface"
            );
        }
        return Result<void>();
    }
};

} // namespace trade_ngin