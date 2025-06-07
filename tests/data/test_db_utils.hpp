// test_db_utils.hpp
#pragma once

#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include <arrow/api.h>
#include <arrow/status.h>
#include <arrow/util/logging.h>
#include <pqxx/pqxx>
#include <memory>
#include <vector>
#include <string>

namespace trade_ngin {
namespace testing {

// ================= Test Data Factories =================
std::shared_ptr<arrow::Table> create_test_market_data();
std::vector<ExecutionReport> create_test_executions();
std::vector<Position> create_test_positions();

// ================= Mock Connection/Transaction =================
class MockConnection {
public:
    explicit MockConnection(const std::string& connection_string)
        : connection_string_(connection_string), is_open_(true) {}
    
    bool is_open() const { return is_open_; }
    void close() { is_open_ = false; }
    const std::string& connection_string() const { return connection_string_; }

private:
    std::string connection_string_;
    bool is_open_;
};

class MockTransaction {
public:
    explicit MockTransaction(MockConnection& conn) : conn_(conn) {}
    
    template<typename... Args>
    pqxx::result exec(const std::string& query, Args&&... args) {
        last_query_ = query;
        return pqxx::result();
    }

    const std::string& last_query() const { return last_query_; }
    void commit() {}

private:
    MockConnection& conn_;
    std::string last_query_;
};

// ================= Mock Postgres Database =================
class MockPostgresDatabase : public PostgresDatabase {
public:
    explicit MockPostgresDatabase(std::string connection_string) 
        : PostgresDatabase(std::move(connection_string)), connected_(false) {}

    // Connection management
    Result<void> connect() override {
        connected_ = true;
        return Result<void>();
    }

    void disconnect() override {
        connected_ = false;
    }

    bool is_connected() const override {
        return connected_;
    }

    // Market data operations
    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override {
            // Validate date range
            if (start_date > end_date) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Start date after end date"
                );
            }

            if (!connected_) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::DATABASE_ERROR,
                    "Not connected to database"
                );
            }
        
            return create_test_market_data();
    }

    // Execution storage
    Result<void> store_executions(
        const std::vector<ExecutionReport>& executions,
        const std::string& table_name) override {
            if (!connected_) return make_error<void>(
                ErrorCode::DATABASE_ERROR, 
                "Not connected"
            );

            if (table_name != "trading.executions") {
                return make_error<void>(
                    ErrorCode::DATABASE_ERROR, 
                    "Invalid table");
                }
                
            return Result<void>();
    }

    // Position storage
    Result<void> store_positions(
        const std::vector<Position>& positions,
        const std::string& table_name) override {
            if (!connected_) return make_error<void>(
                ErrorCode::DATABASE_ERROR, 
                "Not connected"
            );

            if (table_name != "trading.positions") {
                return make_error<void>(
                    ErrorCode::DATABASE_ERROR, 
                    "Invalid table"
                );
            }

            // Simulate error if any position has an invalid symbol (e.g., too long)
            for (const auto& pos : positions) {
                if (pos.symbol.size() > 10) { // Example validation
                    simulate_error_ = true;
                    return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid symbol");
                }
            }

            mock_positions_ = positions;
            simulate_error_ = false;
            return Result<void>();
    }

    // Signal storage
    Result<void> store_signals(
        const std::unordered_map<std::string, double>& signals,
        const std::string& strategy_id,
        const Timestamp& timestamp,
        const std::string& table_name) override {
            if (!connected_) return make_error<void>(
                ErrorCode::DATABASE_ERROR, 
                "Not connected"
            );
        
            return Result<void>();
    }

    // Symbol retrieval
    Result<std::vector<std::string>> get_symbols(
        AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override {
        
        return Result<std::vector<std::string>>({"AAPL", "GOOG"});
    }

    // Query execution
    Result<std::shared_ptr<arrow::Table>> execute_query(
        const std::string& query) override {
            // Check connection state first
            if (!connected_) {
                return make_error<std::shared_ptr<arrow::Table>>(
                    ErrorCode::DATABASE_ERROR,
                    "Not connected to database"
                );
            }
            
            if (query.find("COUNT(*)") != std::string::npos) {
                // Simulate COUNT(*) result
                arrow::Int64Builder count_builder;
                ARROW_CHECK_OK(count_builder.Append(mock_positions_.size()));
                std::shared_ptr<arrow::Array> count_array;
                ARROW_CHECK_OK(count_builder.Finish(&count_array));
                auto schema = arrow::schema({arrow::field("count", arrow::int64())});
                return arrow::Table::Make(schema, {count_array});
            }
            
            return create_test_market_data();
        }

private:
    bool connected_;
    std::vector<Position> mock_positions_;
    bool simulate_error_ = false;
};

} // namespace testing
} // namespace trade_ngin