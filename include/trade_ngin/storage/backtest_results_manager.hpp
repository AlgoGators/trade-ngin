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
    // The typed cells of the backtest.results row (migration 031); a cell with no value is NULL.
    std::vector<LiveResultsCell> summary_cells_;
    std::vector<std::pair<Timestamp, double>> equity_curve_;
    // One entry per equity point (the row's risk_detail object as text, empty for NULL), or
    // empty: the column is not named (migration 020).
    std::vector<std::string> equity_risk_detail_;
    // One list of typed cells per equity point (migration 031: the six overlay columns), or
    // empty: no cell column is named.
    BacktestCurveCells equity_curve_cells_;

    // Multi-strategy support: per-strategy data
    std::unordered_map<std::string, std::vector<ExecutionReport>> strategy_executions_;

    // Metadata
    Timestamp start_date_;
    Timestamp end_date_;
    nlohmann::json hyperparameters_;
    std::string run_name_;
    std::string run_description_;
    std::string portfolio_id_;

public:
    BacktestResultsManager(std::shared_ptr<PostgresDatabase> db,
                          bool store_enabled,
                          const std::string& strategy_id,
                          const std::string& portfolio_id = "BASE_PORTFOLIO");

    ~BacktestResultsManager() override = default;

    // Main storage method - saves everything in correct order
    Result<void> save_all_results(const std::string& run_id,
                                 const Timestamp& date) override;

    // Set data to be stored
    void set_performance_metrics(const std::unordered_map<std::string, double>& metrics) {
        performance_metrics_ = metrics;
    }

    /// The statistics of migration 031 as typed cells (backtest_statistics_cells).
    void set_summary_cells(const std::vector<LiveResultsCell>& cells) {
        summary_cells_ = cells;
    }

    void set_equity_risk_detail(const std::vector<std::string>& risk_detail) {
        equity_risk_detail_ = risk_detail;
    }

    /// The typed cells of the equity-curve rows, one list a point or none.
    void set_equity_curve_cells(const BacktestCurveCells& cells) {
        equity_curve_cells_ = cells;
    }

    void set_equity_curve(const std::vector<std::pair<Timestamp, double>>& curve) {
        equity_curve_ = curve;
    }

    // Multi-strategy support: per-strategy executions
    void set_strategy_executions(const std::string& strategy_id, const std::vector<ExecutionReport>& executions) {
        strategy_executions_[strategy_id] = executions;
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

    // Multi-strategy storage methods
    Result<void> save_strategy_executions(const std::string& portfolio_run_id);
    Result<void> save_strategy_metadata(const std::string& portfolio_run_id,
                                       const std::unordered_map<std::string, double>& strategy_allocations,
                                       const nlohmann::json& portfolio_config);
};

} // namespace trade_ngin