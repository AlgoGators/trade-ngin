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

class PostgresDatabase : public DatabaseInterface {
public:
    explicit PostgresDatabase(std::string connection_string);
    ~PostgresDatabase() override;

    // Delete copy and move operations
    PostgresDatabase(const PostgresDatabase&) = delete;
    PostgresDatabase& operator=(const PostgresDatabase&) = delete;
    PostgresDatabase(PostgresDatabase&&) = delete;
    PostgresDatabase& operator=(PostgresDatabase&&) = delete;

    // DatabaseInterface implementations
    Result<void> connect() override;
    void disconnect() override;
    bool is_connected() const override;

    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override;

    Result<void> store_executions(
        const std::vector<ExecutionReport>& executions,
        const std::string& table_name) override;

    Result<void> store_positions(
        const std::vector<Position>& positions,
        const std::string& table_name) override;

    Result<void> store_signals(
        const std::unordered_map<std::string, double>& signals,
        const std::string& strategy_id,
        const Timestamp& timestamp,
        const std::string& table_name) override;

    Result<std::vector<std::string>> get_symbols(
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override;

    Result<std::shared_ptr<arrow::Table>> execute_query(
        const std::string& query) override;
    
    std::string get_connection_string() const { return connection_string_; }

private:
    std::string connection_string_;
    std::unique_ptr<pqxx::connection> connection_;
    std::mutex mutex_;

    // Helper methods
    Result<void> validate_connection() const;
    std::string format_timestamp(const Timestamp& ts) const;
    std::string side_to_string(Side side) const;

    std::string build_market_data_query(
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& data_type) const;

    Result<std::shared_ptr<arrow::Table>> convert_to_arrow_table(
        const pqxx::result& result) const;

    Result<Timestamp> get_latest_data_time(
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& data_type = "ohlcv") const;

    Result<std::pair<Timestamp, Timestamp>> get_data_time_range(
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& data_type = "ohlcv") const;

    Result<size_t> get_data_count(
        AssetClass asset_class,
        DataFrequency freq,
        const std::string& symbol,
        const std::string& data_type = "ohlcv") const;
};

} // namespace trade_ngin