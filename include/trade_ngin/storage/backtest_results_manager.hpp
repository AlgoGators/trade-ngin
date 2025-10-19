// include/trade_ngin/storage/backtest_results_manager.hpp
// Manages storage of backtest results to database
#pragma once

#include "trade_ngin/storage/results_manager_base.hpp"
#include <chrono>
#include <map>

namespace trade_ngin {

// Forward declarations
class BacktestEngine;

/**
 * @brief Manages storage of backtest results to database
 *
 * Replaces the fragmented save_results_to_db() logic in BacktestEngine.
 * Provides centralized storage for all backtest-related data.
 */
class BacktestResultsManager : public ResultsManagerBase {
private:
    // Cached data for storage
    std::unordered_map<std::string, double> performance_metrics_;
    std::vector<std::pair<Timestamp, double>> equity_curve_;
    std::vector<Position> final_positions_;
    std::vector<ExecutionReport> executions_;
    std::map<Timestamp, std::unordered_map<std::string, double>> signals_history_;

    // Metadata
    Timestamp start_date_;
    Timestamp end_date_;
    nlohmann::json hyperparameters_;
    std::string run_name_;
    std::string run_description_;

public:
    BacktestResultsManager(std::shared_ptr<PostgresDatabase> db,
                          bool store_enabled,
                          const std::string& strategy_id);

    ~BacktestResultsManager() override = default;

    // Main storage method - saves everything in correct order
    Result<void> save_all_results(const std::string& run_id,
                                 const Timestamp& date) override;

    // Set data to be stored
    void set_performance_metrics(const std::unordered_map<std::string, double>& metrics) {
        performance_metrics_ = metrics;
    }

    void set_equity_curve(const std::vector<std::pair<Timestamp, double>>& curve) {
        equity_curve_ = curve;
    }

    void set_final_positions(const std::vector<Position>& positions) {
        final_positions_ = positions;
    }

    void set_executions(const std::vector<ExecutionReport>& executions) {
        executions_ = executions;
    }

    void add_signals(const Timestamp& timestamp,
                    const std::unordered_map<std::string, double>& signals) {
        signals_history_[timestamp] = signals;
    }

    void set_metadata(const Timestamp& start_date,
                     const Timestamp& end_date,
                     const nlohmann::json& hyperparameters,
                     const std::string& run_name = "",
                     const std::string& run_description = "") {
        start_date_ = start_date;
        end_date_ = end_date;
        hyperparameters_ = hyperparameters;
        run_name_ = run_name;
        run_description_ = run_description;
    }

    // Individual storage methods (called by save_all_results)
    Result<void> save_summary_results(const std::string& run_id);
    Result<void> save_equity_curve(const std::string& run_id);
    Result<void> save_final_positions(const std::string& run_id);
    Result<void> save_executions_batch(const std::string& run_id);
    Result<void> save_signals_batch(const std::string& run_id);
    Result<void> save_metadata(const std::string& run_id);

    // Utility method to generate run_id if not provided
    static std::string generate_run_id(const std::string& strategy_id);
};

} // namespace trade_ngin