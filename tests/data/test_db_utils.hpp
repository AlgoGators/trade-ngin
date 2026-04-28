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
        (void)(... , (void)args);
        return pqxx::result();
    }

    const std::string& last_query() const {
        return last_query_;
    }
    void commit() {}

private:
    [[maybe_unused]] MockConnection& conn_;
    std::string last_query_;
};

// ================= Mock Postgres Database =================
class MockPostgresDatabase : public PostgresDatabase {
public:
    explicit MockPostgresDatabase(std::string connection_string)
        : PostgresDatabase(std::move(connection_string)),
          connected_(false),
          simulate_error_(false) {}

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
        (void)symbols; (void)asset_class; (void)freq; (void)data_type;
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
                                  const std::string& strategy_id, const std::string& strategy_name,
                                  const std::string& portfolio_id,
                                  const std::string& table_name) override {
        (void)executions; (void)strategy_id; (void)strategy_name; (void)portfolio_id;
        if (!connected_)
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        if (table_name != "trading.executions") {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid table");
        }
        return record_call("store_executions");
    }

    // Position storage
    Result<void> store_positions(const std::vector<Position>& positions,
                                 const std::string& strategy_id, const std::string& strategy_name,
                                 const std::string& portfolio_id,
                                 const std::string& table_name) override {
        (void)strategy_id; (void)strategy_name; (void)portfolio_id;
        if (!connected_)
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        if (table_name != "trading.positions") {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid table");
        }
        for (const auto& pos : positions) {
            if (pos.symbol.size() > 10) {
                simulate_error_ = true;
                return make_error<void>(ErrorCode::DATABASE_ERROR, "Invalid symbol");
            }
        }
        mock_positions_ = positions;
        simulate_error_ = false;
        return record_call("store_positions");
    }

    // Signal storage
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string& strategy_id, const std::string& strategy_name,
                               const std::string& portfolio_id, const Timestamp& timestamp,
                               const std::string& table_name) override {
        (void)signals;
        (void)strategy_id;
        (void)strategy_name;
        (void)portfolio_id;
        (void)timestamp;
        (void)table_name;
        if (!connected_)
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");

        return Result<void>();
    }

    // Symbol retrieval
    Result<std::vector<std::string>> get_symbols(AssetClass asset_class,
                                                 DataFrequency freq = DataFrequency::DAILY,
                                                 const std::string& data_type = "ohlcv") override {
        (void)asset_class;
        (void)freq;
        (void)data_type;
        return Result<std::vector<std::string>>({"AAPL", "GOOG"});
    }

    // Query execution
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string& query) override {
        // Check connection state first
        if (!connected_) {
            return make_error<std::shared_ptr<arrow::Table>>(ErrorCode::DATABASE_ERROR,
                                                             "Not connected");
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
        (void)query;
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    // Backtest data storage methods
    Result<void> store_backtest_executions(const std::vector<ExecutionReport>& executions,
                                           const std::string& run_id,
                                           const std::string& portfolio_id,
                                           const std::string& table_name) override {
        (void)executions; (void)run_id; (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_executions");
    }

    Result<void> store_backtest_signals(const std::unordered_map<std::string, double>& signals,
                                        const std::string& strategy_id, const std::string& run_id,
                                        const Timestamp& timestamp, const std::string& portfolio_id,
                                        const std::string& table_name) override {
        (void)signals; (void)strategy_id; (void)run_id;
        (void)timestamp; (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_signals");
    }

    Result<void> store_backtest_metadata(const std::string& run_id, const std::string& name,
                                         const std::string& description,
                                         const Timestamp& start_date, const Timestamp& end_date,
                                         const nlohmann::json& hyperparameters,
                                         const std::string& portfolio_id,
                                         const std::string& table_name) override {
        (void)run_id; (void)name; (void)description;
        (void)start_date; (void)end_date; (void)hyperparameters;
        (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_metadata");
    }

    // Live trading data storage methods
    Result<void> store_trading_results(
        const std::string& strategy_id, const Timestamp& date, double total_return,
        double sharpe_ratio, double sortino_ratio, double max_drawdown, double calmar_ratio,
        double volatility, int total_trades, double win_rate, double profit_factor, double avg_win,
        double avg_loss, double max_win, double max_loss, double avg_holding_period, double var_95,
        double cvar_95, double beta, double correlation, double downside_volatility,
        const nlohmann::json& config, const std::string& table_name) override {
        (void)strategy_id;
        (void)date;
        (void)total_return;
        (void)sharpe_ratio;
        (void)sortino_ratio;
        (void)max_drawdown;
        (void)calmar_ratio;
        (void)volatility;
        (void)total_trades;
        (void)win_rate;
        (void)profit_factor;
        (void)avg_win;
        (void)avg_loss;
        (void)max_win;
        (void)max_loss;
        (void)avg_holding_period;
        (void)var_95;
        (void)cvar_95;
        (void)beta;
        (void)correlation;
        (void)downside_volatility;
        (void)config;
        (void)table_name;
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        return Result<void>();
    }

    Result<void> store_trading_equity_curve(const std::string& strategy_id,
                                            const Timestamp& timestamp, double equity,
                                            const std::string& portfolio_id,
                                            const std::string& table_name) override {
        (void)strategy_id; (void)timestamp; (void)equity;
        (void)portfolio_id; (void)table_name;
        return record_call("store_trading_equity_curve");
    }

    Result<void> store_trading_equity_curve_batch(
        const std::string& strategy_id,
        const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& portfolio_id, const std::string& table_name) override {
        (void)strategy_id; (void)equity_points; (void)portfolio_id; (void)table_name;
        return record_call("store_trading_equity_curve_batch");
    }

    // ===== Overrides for newly-virtual methods (storage layer mocking) =====
    //
    // Tests use call_counts_ to assert which methods were invoked and how often.
    // Callers can inject a forced-error symbol via fail_on_call_for_ to test
    // error-propagation paths in storage code.

    Result<void> store_backtest_summary(
        const std::string& run_id, const Timestamp& start_date, const Timestamp& end_date,
        const std::unordered_map<std::string, double>& metrics,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.results") override {
        (void)run_id; (void)start_date; (void)end_date; (void)metrics;
        (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_summary");
    }

    Result<void> store_backtest_equity_curve_batch(
        const std::string& run_id,
        const std::vector<std::pair<Timestamp, double>>& equity_points,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.equity_curve") override {
        (void)run_id; (void)equity_points; (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_equity_curve_batch");
    }

    Result<void> store_backtest_positions(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.final_positions") override {
        (void)positions; (void)run_id; (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_positions");
    }

    Result<void> store_backtest_positions_with_strategy(
        const std::vector<Position>& positions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.final_positions") override {
        (void)positions; (void)run_id; (void)strategy_id;
        (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_positions_with_strategy");
    }

    Result<void> store_backtest_executions_with_strategy(
        const std::vector<ExecutionReport>& executions, const std::string& run_id,
        const std::string& strategy_id, const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.executions") override {
        (void)executions; (void)run_id; (void)strategy_id;
        (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_executions_with_strategy");
    }

    Result<void> store_backtest_metadata_with_portfolio(
        const std::string& run_id, const std::string& portfolio_run_id,
        const std::string& strategy_id, double strategy_allocation,
        const nlohmann::json& portfolio_config,
        const std::string& name, const std::string& description,
        const Timestamp& start_date, const Timestamp& end_date,
        const nlohmann::json& hyperparameters,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "backtest.run_metadata") override {
        (void)run_id; (void)portfolio_run_id; (void)strategy_id; (void)strategy_allocation;
        (void)portfolio_config; (void)name; (void)description;
        (void)start_date; (void)end_date; (void)hyperparameters;
        (void)portfolio_id; (void)table_name;
        return record_call("store_backtest_metadata_with_portfolio");
    }

    Result<void> delete_live_results(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results") override {
        (void)strategy_id; (void)date; (void)portfolio_id; (void)table_name;
        return record_call("delete_live_results");
    }

    Result<void> delete_live_equity_curve(
        const std::string& strategy_id, const Timestamp& date,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve") override {
        (void)strategy_id; (void)date; (void)portfolio_id; (void)table_name;
        return record_call("delete_live_equity_curve");
    }

    Result<void> delete_stale_executions(
        const std::vector<std::string>& order_ids, const Timestamp& date,
        const std::string& strategy_name,
        const std::string& table_name = "trading.executions") override {
        (void)order_ids; (void)date; (void)strategy_name; (void)table_name;
        return record_call("delete_stale_executions");
    }

    Result<void> store_live_results_complete(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& metrics,
        const std::unordered_map<std::string, int>& int_metrics,
        const nlohmann::json& config,
        const std::string& portfolio_id = "BASE_PORTFOLIO",
        const std::string& table_name = "trading.live_results") override {
        (void)strategy_id; (void)date; (void)metrics; (void)int_metrics; (void)config;
        (void)portfolio_id; (void)table_name;
        return record_call("store_live_results_complete");
    }

    Result<void> update_live_results(
        const std::string& strategy_id, const Timestamp& date,
        const std::unordered_map<std::string, double>& updates,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.live_results") override {
        (void)strategy_id; (void)date; (void)updates;
        (void)portfolio_id; (void)table_name;
        return record_call("update_live_results");
    }

    Result<void> update_live_equity_curve(
        const std::string& strategy_id, const Timestamp& date, double equity,
        const std::string& portfolio_id,
        const std::string& table_name = "trading.equity_curve") override {
        (void)strategy_id; (void)date; (void)equity;
        (void)portfolio_id; (void)table_name;
        return record_call("update_live_equity_curve");
    }

    Result<std::shared_ptr<arrow::Table>> get_contract_metadata() const override {
        if (!connected_) {
            return make_error<std::shared_ptr<arrow::Table>>(
                ErrorCode::DATABASE_ERROR, "Not connected");
        }
        // Const method: bypasses call_counts_ tracking. Tests that need to
        // observe invocation can scrutinize state separately.
        return create_test_market_data();  // returns a non-empty arrow table
    }

    // ===== Test hooks =====

    /// Read the number of times the named method has been invoked.
    int call_count(const std::string& method) const {
        auto it = call_counts_.find(method);
        return it == call_counts_.end() ? 0 : it->second;
    }

    /// Reset all call counts to zero.
    void reset_call_counts() { call_counts_.clear(); }

    /// Force the named method to return DATABASE_ERROR on its next invocation
    /// (and subsequent ones until cleared via clear_failure).
    void fail_on_call(const std::string& method) { fail_on_call_for_ = method; }
    void clear_failure() { fail_on_call_for_.clear(); }

private:
    Result<void> record_call(const std::string& method) {
        if (!connected_) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "Not connected");
        }
        ++call_counts_[method];
        if (fail_on_call_for_ == method) {
            return make_error<void>(ErrorCode::DATABASE_ERROR,
                                    "Forced failure on " + method);
        }
        return Result<void>();
    }

    bool connected_;
    std::vector<Position> mock_positions_;
    bool simulate_error_;
    mutable std::unordered_map<std::string, int> call_counts_;
    std::string fail_on_call_for_;
};

}  // namespace testing
}  // namespace trade_ngin