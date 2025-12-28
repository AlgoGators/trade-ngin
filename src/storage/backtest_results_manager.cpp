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

    // 4. Save executions (skip if empty - for portfolio runs, we save per-strategy executions separately)
    if (!executions_.empty()) {
        result = save_executions_batch(run_id);
        if (result.is_error()) {
            ERROR("Failed to save executions: " + std::string(result.error()->what()));
            // Non-fatal, continue
        }
    } else {
        DEBUG("Skipping portfolio-level executions (using per-strategy executions instead)");
    }

    // 5. Save signals
    result = save_signals_batch(run_id);
    if (result.is_error()) {
        ERROR("Failed to save signals: " + std::string(result.error()->what()));
        // Non-fatal, continue
    }

    // 6. Save metadata (skip for portfolio runs - we save per-strategy metadata separately)
    // Portfolio runs are identified by run_id containing '&' (multiple strategies)
    if (run_id.find('&') == std::string::npos) {
        result = save_metadata(run_id);
        if (result.is_error()) {
            ERROR("Failed to save metadata: " + std::string(result.error()->what()));
            // Non-fatal
        }
    } else {
        DEBUG("Skipping portfolio-level metadata (using per-strategy metadata instead)");
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
                                      performance_metrics_, portfolio_id_, "backtest.results");
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
                                                 portfolio_id_, "backtest.equity_curve");
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
                                        portfolio_id_, "backtest.final_positions");
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

    // Override base class method to pass portfolio_id for backtest
    if (executions_.empty()) {
        return Result<void>();
    }
    std::string table_name = "backtest.executions";
    return db_->store_backtest_executions(executions_, run_id, portfolio_id_, table_name);
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
                                                 timestamp, portfolio_id_, "backtest.signals");
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
                                       portfolio_id_, "backtest.run_metadata");
}

Result<void> BacktestResultsManager::save_strategy_positions(const std::string& portfolio_run_id) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (strategy_positions_.empty()) {
        DEBUG("No strategy positions to save for portfolio_run_id: " + portfolio_run_id);
        return Result<void>();
    }

    INFO("Saving positions for " + std::to_string(strategy_positions_.size()) + " strategies");

    // Save positions for each strategy separately
    for (const auto& [strategy_id, positions] : strategy_positions_) {
        if (positions.empty()) {
            continue;
        }

        auto result = db_->store_backtest_positions_with_strategy(
            positions, portfolio_run_id, strategy_id, portfolio_id_, "backtest.final_positions");
        
        if (result.is_error()) {
            ERROR("Failed to save positions for strategy " + strategy_id + ": " + 
                  std::string(result.error()->what()));
            // Continue with other strategies
        } else {
            INFO("Saved " + std::to_string(positions.size()) + 
                 " positions for strategy: " + strategy_id);
        }
    }

    return Result<void>();
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