// src/storage/live_results_manager.cpp
// Implementation of live trading results storage manager

#include "trade_ngin/storage/live_results_manager.hpp"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"

namespace trade_ngin {

LiveResultsManager::LiveResultsManager(std::shared_ptr<PostgresDatabase> db, bool store_enabled,
                                       const std::string& strategy_id,
                                       const std::string& portfolio_id,
                                       const std::string& strategy_name)
    : ResultsManagerBase(db, store_enabled, "trading", strategy_id, portfolio_id,
                         strategy_name),
      current_equity_(0.0),
      has_equity_update_(false) {
    INFO("Initialized LiveResultsManager for strategy: " + strategy_id +
         ", name: " + get_strategy_name() + ", portfolio: " + portfolio_id);
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
    (void)run_id;
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

    // Attempt every table even after a failure -- a signals error must not cost us the
    // executions row -- but remember what failed. Before F-J each failure was logged and
    // swallowed, so save_all_results returned success and the runner exited 0 with tables
    // silently missing. Exit 0 now means every table was actually written.
    std::vector<std::string> failed_tables;
    auto attempt = [&](const char* table, Result<void> r) {
        if (r.is_error()) {
            ERROR("Failed to save " + std::string(table) + ": " +
                  std::string(r.error()->what()));
            failed_tables.emplace_back(table);
        }
    };

    // 2. Save positions
    attempt("positions", save_positions_snapshot(date));

    // 3. Save executions
    attempt("executions", save_executions_batch(date));

    // 4. Save signals
    attempt("signals", save_signals_snapshot(date));

    // 5. Save live results (metrics)
    attempt("live_results", save_live_results(date));

    // 6. Save/update equity curve
    if (has_equity_update_) {
        attempt("equity_curve", save_equity_curve(date));
    }

    if (!failed_tables.empty()) {
        std::string joined;
        for (size_t i = 0; i < failed_tables.size(); ++i) {
            if (i > 0) joined += ", ";
            joined += failed_tables[i];
        }
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                "Failed to persist live results table(s): " + joined,
                                component_id_);
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
    auto result =
        db_->delete_live_results(strategy_id_, date, portfolio_id_, "trading.live_results");
    if (result.is_error()) {
        WARN("Failed to delete stale live results: " + std::string(result.error()->what()));
    }

    // Delete stale equity curve entries
    result =
        db_->delete_live_equity_curve(strategy_id_, date, portfolio_id_, "trading.equity_curve");
    if (result.is_error()) {
        WARN("Failed to delete stale equity curve: " + std::string(result.error()->what()));
    }

    // Delete stale executions if we have new ones
    if (!executions_.empty()) {
        std::vector<std::string> order_ids;
        for (const auto& exec : executions_) {
            order_ids.push_back(exec.order_id);
        }

        // The DELETE filters on trading.executions.strategy_name, so it must be given the
        // NAME, not the id. Passing strategy_id_ here matched nothing wherever the two
        // differ -- for equities the rows carry strategy_name 'EQUITY_MEAN_REVERSION' while
        // strategy_id_ is 'LIVE_EQUITY_MEAN_REVERSION' -- so the stale rows survived and the
        // re-insert failed on executions_pkey (portfolio_id, strategy_id, strategy_name,
        // date, exec_id). That made a date impossible to re-run, which breaks both
        // idempotency (protocol 3f) and the replay-the-missed-day remedy for a run gap.
        //
        // strategy_name_ falls back to strategy_id_ when unset, so callers that never
        // populate the name -- the futures runners -- pass exactly what they passed before.
        result = db_->delete_stale_executions(order_ids, date, strategy_name_, portfolio_id_,
                                             "trading.executions");
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
         " double metrics and " + std::to_string(int_metrics_.size()) + " int metrics");

    // Log all metric names and values for debugging
    for (const auto& [name, value] : double_metrics_) {
        DEBUG("  METRIC [double] " + name + " = " + std::to_string(value));
    }
    for (const auto& [name, value] : int_metrics_) {
        DEBUG("  METRIC [int] " + name + " = " + std::to_string(value));
    }

    // A statistic of the since-inception block that has no value is no column of the INSERT:
    // its cell is NULL (T-8D R12, R40). The runner names total_annualized_return itself.
    auto double_metrics = double_metrics_;
    drop_statistics_without_value(double_metrics);

    // Use the new database extension method - pass portfolio_id_ for proper storage
    auto result =
        db_->store_live_results_complete(strategy_id_, date, double_metrics, int_metrics_, config_,
                                         portfolio_id_, "trading.live_results", risk_detail_,
                                         cells_);
    if (result.is_error()) {
        ERROR("store_live_results_complete FAILED: " + std::string(result.error()->what()));
    } else {
        INFO("store_live_results_complete succeeded for " + strategy_id_);
    }
    return result;
}

Result<void> LiveResultsManager::save_equity_curve(const Timestamp& date) {
    if (!store_enabled_ || !has_equity_update_) {
        return Result<void>();
    }

    // An equity value the curve cannot hold (zero, negative, NaN, infinite, or under 1,000:
    // garbage or uninitialised) is REFUSED: no curve row is written and the error is returned,
    // so save_all_results lists equity_curve among the tables that failed (T-8D-2 R47), as the
    // Day T-1 re-write already refuses such a value. The earlier behaviour wrote the latest
    // stored equity of at least 1,000 in its place, with no bound on that row's date: a number
    // with no run behind it, in a table the statistics read, beside a live_results row that
    // kept the invalid value.
    const double equity_to_save = current_equity_;
    if (!storable_equity_curve_value(equity_to_save)) {
        const std::string what =
            "EQUITY_CURVE REFUSED: the equity value " + std::to_string(equity_to_save) + " for " +
            core::format_utc_date(date) + " (" + strategy_id_ + ", " + portfolio_id_ +
            ") is zero, negative, not finite or under 1,000; no trading.equity_curve row is "
            "written for that date";
        ERROR(what);
        return make_error<void>(ErrorCode::INVALID_DATA, what, component_id_);
    }

    INFO("Saving equity curve point: " + std::to_string(equity_to_save));

    auto result = db_->store_trading_equity_curve(strategy_id_, date, equity_to_save, portfolio_id_,
                                                  "trading.equity_curve");

    return result;
}

Result<void> LiveResultsManager::update_live_results(
    const Timestamp& date, const std::unordered_map<std::string, double>& updates,
    size_t* rows_affected, const std::vector<LiveResultsCell>& cells) {
    if (rows_affected) {
        *rows_affected = 0;
    }
    if (!store_enabled_) {
        return Result<void>();
    }

    if (updates.empty() && cells.empty()) {
        return Result<void>();
    }

    INFO("Updating live results with " + std::to_string(updates.size() + cells.size()) +
         " fields");

    // A statistic of the since-inception block that has no value (profit_factor with no losing
    // session, total_annualized_return under a year of sessions, volatility under two returns,
    // a ratio with no denominator) is assigned NULL, as a cell without a value (T-8D R12, R40).
    auto number_updates = updates;
    auto all_cells = cells;
    null_cells_of_statistics_without_value(number_updates, all_cells);

    return db_->update_live_results(strategy_id_, date, number_updates, portfolio_id_,
                                    "trading.live_results", rows_affected, all_cells);
}

Result<void> LiveResultsManager::update_equity_curve(const Timestamp& date, double equity) {
    if (!store_enabled_) {
        return Result<void>();
    }

    INFO("Updating equity curve: " + std::to_string(equity));

    return db_->update_live_equity_curve(strategy_id_, date, equity, portfolio_id_,
                                         "trading.equity_curve");
}

bool LiveResultsManager::needs_finalization(const Timestamp& current_date,
                                            const Timestamp& previous_date) const {
    // Check if dates are different days
    auto curr_time = std::chrono::system_clock::to_time_t(current_date);
    auto prev_time = std::chrono::system_clock::to_time_t(previous_date);

    std::tm curr_tm = *std::gmtime(&curr_time);
    std::tm prev_tm = *std::gmtime(&prev_time);

    return (curr_tm.tm_year != prev_tm.tm_year || curr_tm.tm_mon != prev_tm.tm_mon ||
            curr_tm.tm_mday != prev_tm.tm_mday);
}

}  // namespace trade_ngin