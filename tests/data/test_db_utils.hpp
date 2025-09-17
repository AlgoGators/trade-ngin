// test_db_utils.hpp
#pragma once

#include <arrow/api.h>
#include <arrow/status.h>
#include <arrow/util/logging.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>
#include <vector>
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/postgres_database.hpp"

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

    bool is_open() const {
        return is_open_;
    }
    void close() {
        is_open_ = false;
    }
    const std::string& connection_string() const {
        return connection_string_;
    }

private:
    std::string connection_string_;
    bool is_open_;
};

class MockTransaction {
public:
    explicit MockTransaction(MockConnection& conn) : conn_(conn) {}

    template <typename... Args>
    pqxx::result exec(const std::string& query, Args&&... args) {
        last_query_ = query;
        return pqxx::result();
    }

    const std::string& last_query() const {
        return last_query_;
    }
    void commit() {}

private:
    MockConnection& conn_;
    std::string last_query_;
};

// ================= Mock Postgres Database =================
class MockPostgresDatabase : public PostgresDatabase {
public:
    explicit MockPostgresDatabase(std::string connection_string)
        : PostgresDatabase(std::move(connection_string)), connected_(false), simulate_error_(false) {}

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
        const std::vector<std::string>& symbols, const Timestamp& start_date,
        const Timestamp& end_date, AssetClass asset_class,
        DataFrequency freq = DataFrequency::DAILY,
        const std::string& data_type = "ohlcv") override {
        // Validate date range
        if (start_date > end_date) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::INVALID_ARGUMENT,
                                                             "Start date after end date");
        }

        if (!connected_) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,
                                                             "Not connected to database");
        }

        return create_test_market_data();
    }

    // Execution storage
    Result<void> store_executions(const std::vector<ExecutionReport>& executions,
                                  const std::string& table_name) override {
        if (!connected_)
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");

        if (table_name != "trading.executions") {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid table");
        }

        return Result<void>();
    }

    // Position storage
    Result<void> store_positions(const std::vector<Position>& positions,
                                 const std::string& strategy_id,
                                 const std::string& table_name) override {
        if (!connected_)
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");

        if (table_name != "trading.positions") {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid table");
        }

        // Simulate error if any position has an invalid symbol (e.g., too long)
        for (const auto& pos : positions) {
            if (pos.symbol.size() > 10) {  // Example validation
                simulate_error_ = true;
                return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid symbol");
            }
        }

        mock_positions_ = positions;
        simulate_error_ = false;
        return Result<void>();
    }

    // Signal storage
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string& strategy_id, const Timestamp& timestamp,
                               const std::string& table_name) override {
        if (!connected_)
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");

        return Result<void>();
    }

    // Symbol retrieval
    Result<std::vector<std::string>> get_symbols(AssetClass asset_class,
                                                 DataFrequency freq = DataFrequency::DAILY,
                                                 const std::string& data_type = "ohlcv") override {
        return Result<std::vector<std::string>>({"AAPL", "GOOG"});
    }

    // Query execution
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        // Check connection state first
        if (!connected_) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR, "Not connected");
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

    // Direct query execution
    Result<void> execute_direct_query(const std::string& query) {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    // Backtest data storage methods
    Result<void> store_backtest_executions(const std::vector<ExecutionReport>& executions,
                                           const std::string& run_id,
                                           const std::string& table_name) override {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    Result<void> store_backtest_signals(const std::unordered_map<std::string, double>& signals,
                                         const std::string& strategy_id, const std::string& run_id,
                                         const Timestamp& timestamp,
                                         const std::string& table_name) override {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    Result<void> store_backtest_metadata(const std::string& run_id, const std::string& name,
                                         const std::string& description, const Timestamp& start_date,
                                         const Timestamp& end_date, const nlohmann::json& hyperparameters,
                                         const std::string& table_name) override {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    // Live trading data storage methods
    Result<void> store_trading_results(const std::string& strategy_id, const Timestamp& date,
                                       double total_return, double sharpe_ratio, double sortino_ratio,
                                       double max_drawdown, double calmar_ratio, double volatility,
                                       int total_trades, double win_rate, double profit_factor,
                                       double avg_win, double avg_loss, double max_win, double max_loss,
                                       double avg_holding_period, double var_95, double cvar_95,
                                       double beta, double correlation, double downside_volatility,
                                       const nlohmann::json& config,
                                       const std::string& table_name) override {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    Result<void> store_trading_equity_curve(const std::string& strategy_id, const Timestamp& timestamp,
                                             double equity,
                                             const std::string& table_name) override {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    Result<void> store_trading_equity_curve_batch(const std::string& strategy_id,
                                                  const std::vector<std::pair<Timestamp, double>>& equity_points,
                                                  const std::string& table_name) override {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

private:
    bool connected_;
    std::vector<Position> mock_positions_;
    bool simulate_error_;
};

}  // namespace testing
}  // namespace trade_ngin