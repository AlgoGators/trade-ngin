// src/storage/backtest_results_manager.cpp
// Implementation of backtest results storage manager

#include "trade_ngin/storage/backtest_results_manager.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace trade_ngin {

BacktestResultsManager::BacktestResultsManager(std::shared_ptr<PostgresDatabase> db,
                                             bool store_enabled,
                                             const std::string& strategy_id)
    : ResultsManagerBase(db, store_enabled, "backtest", strategy_id),
      start_date_(std::chrono::system_clock::now()),
      end_date_(std::chrono::system_clock::now()) {

    INFO("Initialized BacktestResultsManager for strategy: " + strategy_id);
}

std::string BacktestResultsManager::generate_run_id(const std::string& strategy_id) {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << strategy_id << "_";
    ss << std::put_time(std::gmtime(&time_t), "%Y%m%d_%H%M%S");

    // Add milliseconds for uniqueness
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    ss << "_" << std::setfill('0') << std::setw(3) << ms;

    return ss.str();
}

Result<void> BacktestResultsManager::save_all_results(const std::string& run_id,
                                                     const Timestamp& date) {
    // Validate storage is enabled
    auto validation = validate_storage_enabled();
    if (validation.is_error()) {
        INFO("Backtest storage is disabled, skipping save_all_results");
        return Result<void>();  // Not an error, just skip
    }

    validation = validate_database_connection();
    if (validation.is_error()) {
        return validation;
    }

    INFO("Saving all backtest results for run_id: " + run_id);

    // Save in correct order due to foreign key constraints
    // 1. First save summary (creates the run_id entry)
    auto result = save_summary_results(run_id);
    if (result.is_error()) {
        ERROR("Failed to save summary results: " + std::string(result.error()->what()));
        return result;
    }

    // 2. Save equity curve (references run_id)
    result = save_equity_curve(run_id);
    if (result.is_error()) {
        ERROR("Failed to save equity curve: " + std::string(result.error()->what()));
        // Non-fatal, continue with other saves
    }

    // 3. Save final positions (references run_id)
    result = save_final_positions(run_id);
    if (result.is_error()) {
        ERROR("Failed to save final positions: " + std::string(result.error()->what()));
        // Non-fatal, continue
    }

    // 4. Save executions
    result = save_executions_batch(run_id);
    if (result.is_error()) {
        ERROR("Failed to save executions: " + std::string(result.error()->what()));
        // Non-fatal, continue
    }

    // 5. Save signals
    result = save_signals_batch(run_id);
    if (result.is_error()) {
        ERROR("Failed to save signals: " + std::string(result.error()->what()));
        // Non-fatal, continue
    }

    // 6. Save metadata
    result = save_metadata(run_id);
    if (result.is_error()) {
        ERROR("Failed to save metadata: " + std::string(result.error()->what()));
        // Non-fatal
    }

    INFO("Successfully saved all backtest results for run_id: " + run_id);
    return Result<void>();
}

Result<void> BacktestResultsManager::save_summary_results(const std::string& run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (performance_metrics_.empty()) {
        WARN("No performance metrics to save for run_id: " + run_id);
        return Result<void>();
    }

    INFO("Saving backtest summary results for run_id: " + run_id);

    // Use the new database extension method
    return db_->store_backtest_summary(run_id, start_date_, end_date_,
                                      performance_metrics_, "backtest.results");
}

Result<void> BacktestResultsManager::save_equity_curve(const std::string& run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (equity_curve_.empty()) {
        DEBUG("No equity curve data to save for run_id: " + run_id);
        return Result<void>();
    }

    INFO("Saving " + std::to_string(equity_curve_.size()) +
         " equity curve points for run_id: " + run_id);

    // Use the new database extension method
    return db_->store_backtest_equity_curve_batch(run_id, equity_curve_,
                                                 "backtest.equity_curve");
}

Result<void> BacktestResultsManager::save_final_positions(const std::string& run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (final_positions_.empty()) {
        DEBUG("No final positions to save for run_id: " + run_id);
        return Result<void>();
    }

    INFO("Saving " + std::to_string(final_positions_.size()) +
         " final positions for run_id: " + run_id);

    // Use the new database extension method
    return db_->store_backtest_positions(final_positions_, run_id,
                                        "backtest.final_positions");
}

Result<void> BacktestResultsManager::save_executions_batch(const std::string& run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (executions_.empty()) {
        DEBUG("No executions to save for run_id: " + run_id);
        return Result<void>();
    }

    INFO("Saving " + std::to_string(executions_.size()) +
         " executions for run_id: " + run_id);

    // Use the base class method which will route to appropriate storage
    return save_executions(executions_, run_id, end_date_);
}

Result<void> BacktestResultsManager::save_signals_batch(const std::string& run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (signals_history_.empty()) {
        DEBUG("No signals to save for run_id: " + run_id);
        return Result<void>();
    }

    INFO("Saving signals for " + std::to_string(signals_history_.size()) +
         " timestamps for run_id: " + run_id);

    // Save signals for each timestamp
    for (const auto& [timestamp, signals] : signals_history_) {
        auto result = db_->store_backtest_signals(signals, strategy_id_, run_id,
                                                 timestamp, "backtest.signals");
        if (result.is_error()) {
            ERROR("Failed to save signals for timestamp: " +
                  std::to_string(std::chrono::system_clock::to_time_t(timestamp)));
            // Continue with other timestamps
        }
    }

    return Result<void>();
}

Result<void> BacktestResultsManager::save_metadata(const std::string& run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (hyperparameters_.empty()) {
        DEBUG("No metadata to save for run_id: " + run_id);
        return Result<void>();
    }

    INFO("Saving metadata for run_id: " + run_id);

    // Use existing database method
    return db_->store_backtest_metadata(run_id, run_name_, run_description_,
                                       start_date_, end_date_, hyperparameters_,
                                       "backtest.run_metadata");
}

} // namespace trade_ngin