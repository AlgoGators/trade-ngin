// src/live/live_trading_coordinator.cpp
// Implementation of the live trading coordinator

#include "trade_ngin/live/live_trading_coordinator.hpp"
#include <iomanip>
#include <sstream>
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

namespace trade_ngin {

LiveTradingCoordinator::LiveTradingCoordinator(std::shared_ptr<PostgresDatabase> db,
                                               InstrumentRegistry& registry,
                                               const LiveTradingConfig& config)
    : config_(config), db_(db), registry_(&registry) {
    if (!db_) {
        throw std::invalid_argument("Database connection cannot be null");
    }
}

LiveTradingCoordinator::~LiveTradingCoordinator() {
    // Components will be destroyed automatically via unique_ptr
    // This ensures proper cleanup order
}

Result<void> LiveTradingCoordinator::initialize() {
    try {
        // Validate database connection
        auto validation = validate_connection();
        if (validation.is_error()) {
            return validation;
        }

        // Initialize LiveDataLoader
        data_loader_ = std::make_unique<LiveDataLoader>(db_, config_.schema);

        // Initialize LiveMetricsCalculator
        metrics_calculator_ = std::make_unique<LiveMetricsCalculator>();

        // Initialize LiveResultsManager
        results_manager_ = std::make_unique<LiveResultsManager>(
            db_, config_.store_results, config_.strategy_id, config_.portfolio_id,
            config_.strategy_name);

        // Initialize LivePriceManager
        price_manager_ = std::make_unique<LivePriceManager>();

        // Initialize LivePnLManager with InstrumentRegistry
        pnl_manager_ = std::make_unique<LivePnLManager>(config_.initial_capital, *registry_);

        is_initialized_ = true;

        INFO("LiveTradingCoordinator initialized successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                std::string("Failed to initialize coordinator: ") + e.what(),
                                "LiveTradingCoordinator");
    }
}

Result<void> LiveTradingCoordinator::store_results(const TradingMetrics& metrics,
                                                   const std::vector<Position>& positions,
                                                   const Timestamp& date) {
    if (!is_initialized_) {
        return make_error<void>(ErrorCode::NOT_INITIALIZED, "Coordinator not initialized",
                                "LiveTradingCoordinator");
    }

    if (!config_.store_results) {
        // Storage disabled
        return Result<void>();
    }

    try {
        // Set metrics in results manager
        std::unordered_map<std::string, double> double_metrics = {
            {"total_cumulative_return", metrics.total_cumulative_return},
            {"total_annualized_return", metrics.total_annualized_return},
            {"total_pnl", metrics.total_pnl},
            {"total_unrealized_pnl", metrics.unrealized_pnl},
            {"total_realized_pnl", metrics.realized_pnl},
            {"current_portfolio_value", metrics.current_portfolio_value},
            {"portfolio_leverage", metrics.gross_leverage},
            {"equity_to_margin_ratio", metrics.equity_to_margin_ratio},
            {"margin_cushion", metrics.margin_cushion},
            {"gross_notional", metrics.gross_notional},
            {"margin_posted", metrics.margin_posted},
            {"daily_return", metrics.daily_return},
            {"daily_pnl", metrics.daily_pnl}};

        std::unordered_map<std::string, int> int_metrics = {
            {"active_positions", static_cast<int>(positions.size())},
            {"trading_days", metrics.trading_days}};

        results_manager_->set_metrics(double_metrics, int_metrics);
        results_manager_->set_positions(positions);

        // Store all results
        auto store_result = results_manager_->save_all_results(config_.strategy_id, date);
        if (store_result.is_error()) {
            return make_error<void>(
                ErrorCode::DATABASE_ERROR,
                std::string("Failed to store results: ") + store_result.error()->what(),
                "LiveTradingCoordinator");
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(ErrorCode::DATABASE_ERROR,
                                std::string("Failed to store results: ") + e.what(),
                                "LiveTradingCoordinator");
    }
}

// Drift-D: the COORDINATOR's load_commissions_by_symbol() wrapper was deleted as dead code.
// The LiveDataLoader method it wrapped is alive and called every run by
// live_equity_mean_reversion.cpp, so read this as "the wrapper is gone", not "the function
// is gone" -- the previous wording said the latter and sent a reader looking for a deletion
// that never happened. The two futures runners dropped their own calls (the result map was
// unused there); the equity runner still uses the figures for its per-symbol CSV export.

Result<std::vector<Position>> LiveTradingCoordinator::load_positions_for_export(
    const Timestamp& date) const {
    if (!is_initialized_) {
        return make_error<std::vector<Position>>(
            ErrorCode::NOT_INITIALIZED, "Coordinator not initialized", "LiveTradingCoordinator");
    }

    return data_loader_->load_positions_for_export(config_.strategy_id, config_.portfolio_id, date);
}

Result<void> LiveTradingCoordinator::validate_connection() const {
    if (!db_) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR, "Database connection is null",
                                "LiveTradingCoordinator");
    }

    if (!db_->is_connected()) {
        return make_error<void>(ErrorCode::CONNECTION_ERROR, "Database is not connected",
                                "LiveTradingCoordinator");
    }

    return Result<void>();
}

}  // namespace trade_ngin