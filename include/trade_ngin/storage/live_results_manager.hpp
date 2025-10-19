// include/trade_ngin/storage/live_results_manager.hpp
// Manages storage of live trading results to database
#pragma once

#include "trade_ngin/storage/results_manager_base.hpp"

namespace trade_ngin {

/**
 * @brief Manages storage of live trading results to database
 *
 * Provides centralized storage for all live trading data.
 * Handles positions, executions, signals, metrics, and equity curves.
 */
class LiveResultsManager : public ResultsManagerBase {
private:
    // Cached data for storage
    std::vector<Position> positions_;
    std::vector<ExecutionReport> executions_;
    std::unordered_map<std::string, double> signals_;

    // Performance metrics
    std::unordered_map<std::string, double> double_metrics_;
    std::unordered_map<std::string, int> int_metrics_;

    // Configuration
    nlohmann::json config_;

    // Equity tracking
    double current_equity_;
    bool has_equity_update_;

public:
    LiveResultsManager(std::shared_ptr<PostgresDatabase> db,
                      bool store_enabled,
                      const std::string& strategy_id);

    ~LiveResultsManager() override = default;

    // Main storage method - saves everything for the trading day
    Result<void> save_all_results(const std::string& run_id,
                                 const Timestamp& date) override;

    // Set data to be stored
    void set_positions(const std::vector<Position>& positions) {
        positions_ = positions;
    }

    void set_executions(const std::vector<ExecutionReport>& executions) {
        executions_ = executions;
    }

    void set_signals(const std::unordered_map<std::string, double>& signals) {
        signals_ = signals;
    }

    void set_metrics(const std::unordered_map<std::string, double>& double_metrics,
                    const std::unordered_map<std::string, int>& int_metrics = {}) {
        double_metrics_ = double_metrics;
        int_metrics_ = int_metrics;
    }

    void set_config(const nlohmann::json& config) {
        config_ = config;
    }

    void set_equity(double equity) {
        current_equity_ = equity;
        has_equity_update_ = true;
    }

    // Individual storage methods (called by save_all_results)
    Result<void> delete_stale_data(const Timestamp& date);
    Result<void> save_positions_snapshot(const Timestamp& date);
    Result<void> save_executions_batch(const Timestamp& date);
    Result<void> save_signals_snapshot(const Timestamp& date);
    Result<void> save_live_results(const Timestamp& date);
    Result<void> save_equity_curve(const Timestamp& date);

    // Update operations for existing data
    Result<void> update_live_results(const Timestamp& date,
                                    const std::unordered_map<std::string, double>& updates);

    Result<void> update_equity_curve(const Timestamp& date,
                                    double equity);

    // Utility methods
    static std::string generate_run_id(const std::string& strategy_id,
                                       const Timestamp& date);

    // Check if we need to finalize previous day
    bool needs_finalization(const Timestamp& current_date,
                           const Timestamp& previous_date) const;
};

} // namespace trade_ngin