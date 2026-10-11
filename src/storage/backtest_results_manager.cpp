// src/storage/backtest_results_manager.cpp
// Implementation of backtest results storage manager

#include "trade_ngin/storage/backtest_results_manager.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/run_id_generator.hpp"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace trade_ngin {

BacktestResultsManager::BacktestResultsManager(std::shared_ptr<PostgresDatabase> db,
                                             bool store_enabled,
                                             const std::string& strategy_id,
                                             const std::string& portfolio_id)
    : ResultsManagerBase(db, store_enabled, "backtest", strategy_id),
      start_date_(std::chrono::system_clock::now()),
      end_date_(std::chrono::system_clock::now()),
      portfolio_id_(portfolio_id.empty() ? "BASE_PORTFOLIO" : portfolio_id) {

    INFO("Initialized BacktestResultsManager for strategy: " + strategy_id + ", portfolio: " + portfolio_id_);
}

Result<void> BacktestResultsManager::save_all_results(const std::string& run_id,
                                                     const Timestamp& date) {
    (void)date;
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

    // The daily positions are written through the run by
    // BacktestCoordinator::save_daily_positions and the executions per strategy by
    // save_strategy_executions(); backtest.signals has no writer (the table is reserved).

    // Metadata is written per-strategy by BacktestCoordinator::save_portfolio_results_to_db
    // via save_strategy_metadata() (correct ON CONFLICT (run_id, strategy_id)). The portfolio-level
    // pair save_metadata()/store_backtest_metadata() used ON CONFLICT (run_id), which has no
    // matching unique constraint on backtest.run_metadata, and omitted the NOT NULL strategy_id --
    // it could never succeed, and its only caller was a unit test. Both were deleted (F-5).

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
                                      performance_metrics_, portfolio_id_, "backtest.results",
                                      summary_cells_);
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
    return db_->store_backtest_equity_curve_batch(run_id, equity_curve_, portfolio_id_,
                                                  "backtest.equity_curve", equity_risk_detail_,
                                                  equity_curve_cells_);
}

Result<void> BacktestResultsManager::save_strategy_executions(const std::string& portfolio_run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (strategy_executions_.empty()) {
        DEBUG("No strategy executions to save for portfolio_run_id: " + portfolio_run_id);
        return Result<void>();
    }

    INFO("Saving executions for " + std::to_string(strategy_executions_.size()) + " strategies");

    // Save executions for each strategy separately
    for (const auto& [strategy_id, executions] : strategy_executions_) {
        DEBUG("Strategy " + strategy_id + " has " + std::to_string(executions.size()) + " executions to save");
        if (executions.empty()) {
            DEBUG("Skipping empty executions for strategy: " + strategy_id);
            continue;
        }

        INFO("Attempting to save " + std::to_string(executions.size()) + 
             " executions for strategy: " + strategy_id);
        auto result = db_->store_backtest_executions_with_strategy(
            executions, portfolio_run_id, strategy_id, portfolio_id_, "backtest.executions");
        
        if (result.is_error()) {
            ERROR("Failed to save executions for strategy " + strategy_id + ": " + 
                  std::string(result.error()->what()));
            // Continue with other strategies
        } else {
            INFO("Saved " + std::to_string(executions.size()) + 
                 " executions for strategy: " + strategy_id);
        }
    }

    return Result<void>();
}

Result<void> BacktestResultsManager::save_strategy_metadata(
    const std::string& portfolio_run_id,
    const std::unordered_map<std::string, double>& strategy_allocations,
    const nlohmann::json& portfolio_config) {
    
    if (!store_enabled_) {
        return Result<void>();
    }

    // Use portfolio_run_id as the run_id for all strategies to match what's stored in
    // results table and final_positions table. Each strategy will have a separate row
    // differentiated by strategy_id.
    INFO("Saving metadata for " + std::to_string(strategy_allocations.size()) + " strategies");
    
    for (const auto& [strategy_id, allocation] : strategy_allocations) {
        INFO("Processing metadata for strategy: " + strategy_id + " with allocation: " + std::to_string(allocation));
        
        auto result = db_->store_backtest_metadata_with_portfolio(
            portfolio_run_id,          // Use portfolio run_id to match results/final_positions tables
            portfolio_run_id,          // Portfolio run_id (same as run_id)
            strategy_id,                // Strategy ID
            allocation,                 // Strategy allocation
            portfolio_config,           // Portfolio config
            run_name_,                 // Name
            run_description_,          // Description
            start_date_,               // Start date
            end_date_,                 // End date
            hyperparameters_,         // Hyperparameters
            portfolio_id_,             // Portfolio ID
            "backtest.run_metadata");

        if (result.is_error()) {
            ERROR("Failed to save metadata for strategy " + strategy_id + ": " + 
                  std::string(result.error()->what()));
            // Continue with other strategies
        } else {
            INFO("Successfully saved metadata for strategy: " + strategy_id + ", run_id: " + portfolio_run_id);
        }
    }
    
    INFO("Finished saving metadata for all strategies");

    return Result<void>();
}

} // namespace trade_ngin