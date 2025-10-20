// src/storage/live_results_manager.cpp
// Implementation of live trading results storage manager

#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/logger.hpp"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace trade_ngin {

LiveResultsManager::LiveResultsManager(std::shared_ptr<PostgresDatabase> db,
                                     bool store_enabled,
                                     const std::string& strategy_id)
    : ResultsManagerBase(db, store_enabled, "trading", strategy_id),
      current_equity_(0.0),
      has_equity_update_(false) {

    INFO("Initialized LiveResultsManager for strategy: " + strategy_id);
}

std::string LiveResultsManager::generate_run_id(const std::string& strategy_id,
                                               const Timestamp& date) {
    auto time_t = std::chrono::system_clock::to_time_t(date);

    std::stringstream ss;
    ss << strategy_id << "_LIVE_";
    ss << std::put_time(std::gmtime(&time_t), "%Y%m%d");

    return ss.str();
}

Result<void> LiveResultsManager::save_all_results(const std::string& run_id,
                                                 const Timestamp& date) {
    // Validate storage is enabled
    auto validation = validate_storage_enabled();
    if (validation.is_error()) {
        INFO("Live storage is disabled, skipping save_all_results");
        return Result<void>();  // Not an error, just skip
    }

    validation = validate_database_connection();
    if (validation.is_error()) {
        return validation;
    }

    INFO("Saving all live trading results for date: " +
         std::to_string(std::chrono::system_clock::to_time_t(date)));

    // 1. First delete any stale data from previous runs on the same day
    auto result = delete_stale_data(date);
    if (result.is_error()) {
        WARN("Failed to delete stale data: " + std::string(result.error()->what()));
        // Non-fatal, continue
    }

    // 2. Save positions
    result = save_positions_snapshot(date);
    if (result.is_error()) {
        ERROR("Failed to save positions: " + std::string(result.error()->what()));
        // Continue with other saves
    }

    // 3. Save executions
    result = save_executions_batch(date);
    if (result.is_error()) {
        ERROR("Failed to save executions: " + std::string(result.error()->what()));
        // Continue
    }

    // 4. Save signals
    result = save_signals_snapshot(date);
    if (result.is_error()) {
        ERROR("Failed to save signals: " + std::string(result.error()->what()));
        // Continue
    }

    // 5. Save live results (metrics)
    result = save_live_results(date);
    if (result.is_error()) {
        ERROR("Failed to save live results: " + std::string(result.error()->what()));
        // Continue
    }

    // 6. Save/update equity curve
    if (has_equity_update_) {
        result = save_equity_curve(date);
        if (result.is_error()) {
            ERROR("Failed to save equity curve: " + std::string(result.error()->what()));
            // Non-fatal
        }
    }

    INFO("Successfully saved all live trading results for date: " +
         std::to_string(std::chrono::system_clock::to_time_t(date)));

    return Result<void>();
}

Result<void> LiveResultsManager::delete_stale_data(const Timestamp& date) {
    if (!store_enabled_) {
        return Result<void>();
    }

    INFO("Deleting stale data for date: " +
         std::to_string(std::chrono::system_clock::to_time_t(date)));

    // Delete stale live results for re-runs
    auto result = db_->delete_live_results(strategy_id_, date, "trading.live_results");
    if (result.is_error()) {
        WARN("Failed to delete stale live results: " + std::string(result.error()->what()));
    }

    // Delete stale equity curve entries
    result = db_->delete_live_equity_curve(strategy_id_, date, "trading.equity_curve");
    if (result.is_error()) {
        WARN("Failed to delete stale equity curve: " + std::string(result.error()->what()));
    }

    // Delete stale executions if we have new ones
    if (!executions_.empty()) {
        std::vector<std::string> order_ids;
        for (const auto& exec : executions_) {
            order_ids.push_back(exec.order_id);
        }

        result = db_->delete_stale_executions(order_ids, date, "trading.executions");
        if (result.is_error()) {
            WARN("Failed to delete stale executions: " + std::string(result.error()->what()));
        }
    }

    return Result<void>();
}

Result<void> LiveResultsManager::save_positions_snapshot(const Timestamp& date) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (positions_.empty()) {
        DEBUG("No positions to save for date: " +
              std::to_string(std::chrono::system_clock::to_time_t(date)));
        return Result<void>();
    }

    INFO("Saving " + std::to_string(positions_.size()) + " positions");

    // Use base class method which routes to appropriate storage
    return save_positions(positions_, strategy_id_, date);
}

Result<void> LiveResultsManager::save_executions_batch(const Timestamp& date) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (executions_.empty()) {
        DEBUG("No executions to save for date: " +
              std::to_string(std::chrono::system_clock::to_time_t(date)));
        return Result<void>();
    }

    INFO("Saving " + std::to_string(executions_.size()) + " executions");

    // Use base class method which routes to appropriate storage
    return save_executions(executions_, strategy_id_, date);
}

Result<void> LiveResultsManager::save_signals_snapshot(const Timestamp& date) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (signals_.empty()) {
        DEBUG("No signals to save for date: " +
              std::to_string(std::chrono::system_clock::to_time_t(date)));
        return Result<void>();
    }

    INFO("Saving " + std::to_string(signals_.size()) + " signals");

    // Use base class method which routes to appropriate storage
    return save_signals(signals_, strategy_id_, date);
}

Result<void> LiveResultsManager::save_live_results(const Timestamp& date) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (double_metrics_.empty()) {
        WARN("No metrics to save for date: " +
             std::to_string(std::chrono::system_clock::to_time_t(date)));
        return Result<void>();
    }

    INFO("Saving live results with " + std::to_string(double_metrics_.size()) +
         " metrics");

    // Use the new database extension method
    return db_->store_live_results_complete(strategy_id_, date, double_metrics_,
                                           int_metrics_, config_, "trading.live_results");
}

Result<void> LiveResultsManager::save_equity_curve(const Timestamp& date) {
    if (!store_enabled_ || !has_equity_update_) {
        return Result<void>();
    }

    double equity_to_save = current_equity_;

    // Validate equity value - check for invalid values:
    // 1. Zero or negative
    // 2. NaN or infinity
    // 3. Suspiciously small (< $1000 - likely garbage/uninitialized)
    bool is_invalid = (equity_to_save <= 0.0) ||
                      std::isnan(equity_to_save) ||
                      std::isinf(equity_to_save) ||
                      (equity_to_save > 0.0 && equity_to_save < 1000.0);

    if (is_invalid) {
        WARN("Invalid equity value: " + std::to_string(equity_to_save) +
             " (zero, negative, NaN, inf, or suspiciously small). Attempting to use previous day's value");

        // Try to get the most recent valid equity value (>= 1000)
        std::string get_prev_equity_query =
            "SELECT equity FROM trading.equity_curve "
            "WHERE strategy_id = '" + strategy_id_ + "' "
            "AND equity >= 1000.0 "
            "ORDER BY timestamp DESC LIMIT 1";

        auto prev_result = db_->execute_query(get_prev_equity_query);
        if (prev_result.is_ok() && prev_result.value()->num_rows() > 0) {
            auto table = prev_result.value();
            auto array = std::static_pointer_cast<arrow::DoubleArray>(table->column(0)->chunk(0));
            equity_to_save = array->Value(0);
            INFO("Using previous equity value: " + std::to_string(equity_to_save));
        } else {
            ERROR("Cannot save equity curve: equity is invalid (" + std::to_string(current_equity_) +
                  ") and no previous valid value found");
            return Result<void>();  // Don't save invalid values
        }
    }

    INFO("Saving equity curve point: " + std::to_string(equity_to_save));

    // Use store method which handles INSERT ... ON CONFLICT (UPSERT)
    auto result = db_->store_trading_equity_curve(strategy_id_, date,
                                                 equity_to_save, "trading.equity_curve");

    return result;
}

Result<void> LiveResultsManager::update_live_results(const Timestamp& date,
                                                    const std::unordered_map<std::string, double>& updates) {
    if (!store_enabled_) {
        return Result<void>();
    }

    if (updates.empty()) {
        return Result<void>();
    }

    INFO("Updating live results with " + std::to_string(updates.size()) + " fields");

    return db_->update_live_results(strategy_id_, date, updates, "trading.live_results");
}

Result<void> LiveResultsManager::update_equity_curve(const Timestamp& date, double equity) {
    if (!store_enabled_) {
        return Result<void>();
    }

    INFO("Updating equity curve: " + std::to_string(equity));

    return db_->update_live_equity_curve(strategy_id_, date, equity, "trading.equity_curve");
}

bool LiveResultsManager::needs_finalization(const Timestamp& current_date,
                                           const Timestamp& previous_date) const {
    // Check if dates are different days
    auto curr_time = std::chrono::system_clock::to_time_t(current_date);
    auto prev_time = std::chrono::system_clock::to_time_t(previous_date);

    std::tm curr_tm = *std::gmtime(&curr_time);
    std::tm prev_tm = *std::gmtime(&prev_time);

    return (curr_tm.tm_year != prev_tm.tm_year ||
            curr_tm.tm_mon != prev_tm.tm_mon ||
            curr_tm.tm_mday != prev_tm.tm_mday);
}

} // namespace trade_ngin