// src/live/live_trading_coordinator.cpp
// Implementation of the live trading coordinator

#include "trade_ngin/live/live_trading_coordinator.hpp"
#include "trade_ngin/live/live_data_loader.hpp"
#include "trade_ngin/live/live_metrics_calculator.hpp"
#include "trade_ngin/live/live_price_manager.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include <sstream>
#include <iomanip>

namespace trade_ngin {

LiveTradingCoordinator::LiveTradingCoordinator(
    std::shared_ptr<PostgresDatabase> db,
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
            db_, config_.store_results, config_.strategy_id);

        // Initialize LivePriceManager
        price_manager_ = std::make_unique<LivePriceManager>(db_);

        // Initialize LivePnLManager with InstrumentRegistry
        pnl_manager_ = std::make_unique<LivePnLManager>(config_.initial_capital, *registry_);

        is_initialized_ = true;

        INFO("LiveTradingCoordinator initialized successfully");
        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            std::string("Failed to initialize coordinator: ") + e.what(),
            "LiveTradingCoordinator");
    }
}

Result<std::pair<double, TradingMetrics>> LiveTradingCoordinator::load_previous_day_data(
    const Timestamp& date) const {

    if (!is_initialized_) {
        return make_error<std::pair<double, TradingMetrics>>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
            "LiveTradingCoordinator");
    }

    try {
        // Calculate previous date
        auto previous_date = date - std::chrono::hours(24);

        // Load previous day's results
        auto results = data_loader_->load_live_results(config_.strategy_id, previous_date);
        if (results.is_error()) {
            // No previous data, return initial values
            TradingMetrics empty_metrics;
            empty_metrics.current_portfolio_value = config_.initial_capital;
            return Result<std::pair<double, TradingMetrics>>(
                std::make_pair(config_.initial_capital, empty_metrics));
        }

        // Convert to TradingMetrics
        TradingMetrics metrics;
        auto& row = results.value();

        metrics.daily_pnl = row.daily_pnl;
        metrics.total_pnl = row.total_pnl;
        metrics.realized_pnl = row.daily_realized_pnl;
        metrics.unrealized_pnl = row.daily_unrealized_pnl;
        metrics.current_portfolio_value = row.current_portfolio_value;
        metrics.gross_notional = row.gross_notional;
        metrics.margin_posted = row.margin_posted;
        metrics.daily_return = row.daily_return;
        metrics.total_cumulative_return = row.total_cumulative_return;
        metrics.total_annualized_return = row.total_annualized_return;
        metrics.portfolio_leverage = row.portfolio_leverage;
        metrics.equity_to_margin_ratio = row.equity_to_margin_ratio;
        metrics.active_positions = row.active_positions;

        return Result<std::pair<double, TradingMetrics>>(
            std::make_pair(row.current_portfolio_value, metrics));

    } catch (const std::exception& e) {
        return make_error<std::pair<double, TradingMetrics>>(
            ErrorCode::DATABASE_ERROR,
            std::string("Failed to load previous day data: ") + e.what(),
            "LiveTradingCoordinator");
    }
}

Result<TradingMetrics> LiveTradingCoordinator::calculate_daily_metrics(
    double daily_pnl,
    double previous_portfolio_value,
    double current_portfolio_value,
    double gross_notional,
    double margin_posted,
    int trading_days,
    double daily_commissions) {

    if (!is_initialized_) {
        return make_error<TradingMetrics>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
            "LiveTradingCoordinator");
    }

    try {
        // Use LiveMetricsCalculator to calculate all metrics
        auto calc_metrics = metrics_calculator_->calculate_all_metrics(
            daily_pnl,
            previous_portfolio_value,
            current_portfolio_value,
            config_.initial_capital,
            gross_notional,
            margin_posted,
            trading_days,
            daily_commissions
        );

        // Convert to TradingMetrics
        current_metrics_ = convert_calculated_metrics(calc_metrics);

        // Add additional fields not in CalculatedMetrics
        current_metrics_.current_portfolio_value = current_portfolio_value;
        current_metrics_.gross_notional = gross_notional;
        current_metrics_.margin_posted = margin_posted;
        current_metrics_.trading_days = trading_days;

        return Result<TradingMetrics>(current_metrics_);

    } catch (const std::exception& e) {
        return make_error<TradingMetrics>(
            ErrorCode::INVALID_DATA,
            std::string("Failed to calculate daily metrics: ") + e.what(),
            "LiveTradingCoordinator");
    }
}

Result<TradingMetrics> LiveTradingCoordinator::calculate_finalization_metrics(
    double realized_pnl,
    double day_before_portfolio,
    double current_portfolio,
    double gross_notional,
    double margin_posted,
    int trading_days,
    double commissions) {

    if (!is_initialized_) {
        return make_error<TradingMetrics>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
            "LiveTradingCoordinator");
    }

    try {
        // Use LiveMetricsCalculator for finalization
        auto calc_metrics = metrics_calculator_->calculate_finalization_metrics(
            realized_pnl,
            day_before_portfolio,
            current_portfolio,
            config_.initial_capital,
            gross_notional,
            margin_posted,
            trading_days,
            commissions
        );

        // Convert to TradingMetrics
        TradingMetrics metrics = convert_calculated_metrics(calc_metrics);

        // Add additional fields
        metrics.current_portfolio_value = current_portfolio;
        metrics.gross_notional = gross_notional;
        metrics.margin_posted = margin_posted;
        metrics.trading_days = trading_days;

        return Result<TradingMetrics>(metrics);

    } catch (const std::exception& e) {
        return make_error<TradingMetrics>(
            ErrorCode::INVALID_DATA,
            std::string("Failed to calculate finalization metrics: ") + e.what(),
            "LiveTradingCoordinator");
    }
}

Result<void> LiveTradingCoordinator::store_results(
    const TradingMetrics& metrics,
    const std::vector<Position>& positions,
    const Timestamp& date) {

    if (!is_initialized_) {
        return make_error<void>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
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
            {"portfolio_leverage", metrics.portfolio_leverage},
            {"equity_to_margin_ratio", metrics.equity_to_margin_ratio},
            {"margin_cushion", metrics.margin_cushion},
            {"gross_notional", metrics.gross_notional},
            {"margin_posted", metrics.margin_posted},
            {"daily_return", metrics.daily_return},
            {"daily_pnl", metrics.daily_pnl}
        };

        std::unordered_map<std::string, int> int_metrics = {
            {"active_positions", static_cast<int>(positions.size())},
            {"trading_days", metrics.trading_days}
        };

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
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            std::string("Failed to store results: ") + e.what(),
            "LiveTradingCoordinator");
    }
}

Result<std::unordered_map<std::string, double>> LiveTradingCoordinator::load_commissions_by_symbol(
    const Timestamp& date) const {

    if (!is_initialized_) {
        return make_error<std::unordered_map<std::string, double>>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
            "LiveTradingCoordinator");
    }

    return data_loader_->load_commissions_by_symbol(date);
}

Result<std::vector<Position>> LiveTradingCoordinator::load_positions_for_export(
    const Timestamp& date) const {

    if (!is_initialized_) {
        return make_error<std::vector<Position>>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
            "LiveTradingCoordinator");
    }

    return data_loader_->load_positions_for_export(config_.strategy_id, date);
}

Result<int> LiveTradingCoordinator::get_trading_days_count() const {
    if (!is_initialized_) {
        return make_error<int>(
            ErrorCode::NOT_INITIALIZED,
            "Coordinator not initialized",
            "LiveTradingCoordinator");
    }

    return data_loader_->get_live_results_count(config_.strategy_id);
}

Result<void> LiveTradingCoordinator::validate_connection() const {
    if (!db_) {
        return make_error<void>(
            ErrorCode::CONNECTION_ERROR,
            "Database connection is null",
            "LiveTradingCoordinator");
    }

    if (!db_->is_connected()) {
        return make_error<void>(
            ErrorCode::CONNECTION_ERROR,
            "Database is not connected",
            "LiveTradingCoordinator");
    }

    return Result<void>();
}

TradingMetrics LiveTradingCoordinator::convert_calculated_metrics(
    const CalculatedMetrics& calc_metrics) const {

    TradingMetrics metrics;

    // Return metrics
    metrics.daily_return = calc_metrics.daily_return;
    metrics.total_cumulative_return = calc_metrics.total_return;  // total_return from calculator is cumulative
    metrics.total_annualized_return = calc_metrics.annualized_return;

    // Portfolio metrics
    metrics.portfolio_leverage = calc_metrics.portfolio_leverage;
    metrics.equity_to_margin_ratio = calc_metrics.equity_to_margin_ratio;
    metrics.margin_cushion = calc_metrics.margin_cushion;
    metrics.cash_available = calc_metrics.cash_available;

    // PnL metrics
    metrics.daily_pnl = calc_metrics.daily_pnl;
    metrics.total_pnl = calc_metrics.total_pnl;
    metrics.realized_pnl = calc_metrics.realized_pnl;
    metrics.unrealized_pnl = calc_metrics.unrealized_pnl;

    return metrics;
}

} // namespace trade_ngin