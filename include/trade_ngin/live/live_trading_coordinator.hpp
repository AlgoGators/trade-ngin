// include/trade_ngin/live/live_trading_coordinator.hpp
// Coordinator for all live trading components - ensures proper initialization and connection management

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

// Forward declarations
class LiveDataLoader;
class LiveMetricsCalculator;
class LiveResultsManager;
class LivePriceManager;
class LivePnLManager;
class InstrumentRegistry;

/**
 * @brief Configuration for LiveTradingCoordinator
 */
struct LiveTradingConfig {
    std::string strategy_id = "LIVE_TREND_FOLLOWING";
    std::string schema = "trading";
    double initial_capital = 500000.0;
    bool store_results = true;
    bool calculate_risk_metrics = true;
};

/**
 * @brief Aggregated metrics from all components
 */
struct TradingMetrics {
    // From LiveMetricsCalculator
    double daily_return = 0.0;
    double total_cumulative_return = 0.0;  // Total return since inception (non-annualized)
    double total_annualized_return = 0.0;  // Annualized return
    double portfolio_leverage = 0.0;
    double equity_to_margin_ratio = 0.0;
    double margin_cushion = 0.0;
    double cash_available = 0.0;

    // PnL metrics
    double daily_pnl = 0.0;
    double total_pnl = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;

    // Portfolio metrics
    double current_portfolio_value = 0.0;
    double gross_notional = 0.0;
    double margin_posted = 0.0;
    int active_positions = 0;
    int trading_days = 0;
};

/**
 * @brief Coordinator for all live trading components
 *
 * This class manages the lifecycle and coordination of:
 * - LiveDataLoader: Data retrieval from database
 * - LiveMetricsCalculator: Metric calculations
 * - LiveResultsManager: Storage operations
 *
 * It ensures proper RAII, shared database connections, and
 * provides a simplified interface for live_trend.cpp
 */
class LiveTradingCoordinator {
private:
    // Configuration
    LiveTradingConfig config_;

    // Shared database connection
    std::shared_ptr<PostgresDatabase> db_;
    
    // Reference to instrument registry
    InstrumentRegistry* registry_;

    // Managed components
    std::unique_ptr<LiveDataLoader> data_loader_;
    std::unique_ptr<LiveMetricsCalculator> metrics_calculator_;
    std::unique_ptr<LiveResultsManager> results_manager_;
    std::unique_ptr<LivePriceManager> price_manager_;
    std::unique_ptr<LivePnLManager> pnl_manager_;

    // Current state
    TradingMetrics current_metrics_;
    bool is_initialized_ = false;

public:
    /**
     * @brief Constructor
     * @param db Shared database connection
     * @param registry Reference to instrument registry
     * @param config Configuration for the coordinator
     */
    LiveTradingCoordinator(
        std::shared_ptr<PostgresDatabase> db,
        InstrumentRegistry& registry,
        const LiveTradingConfig& config = {});

    /**
     * @brief Destructor
     */
    ~LiveTradingCoordinator();

    // ========== Component Access ==========

    /**
     * @brief Get data loader for direct access if needed
     */
    LiveDataLoader* get_data_loader() { return data_loader_.get(); }
    const LiveDataLoader* get_data_loader() const { return data_loader_.get(); }

    /**
     * @brief Get metrics calculator for direct access if needed
     */
    LiveMetricsCalculator* get_metrics_calculator() { return metrics_calculator_.get(); }
    const LiveMetricsCalculator* get_metrics_calculator() const { return metrics_calculator_.get(); }

    /**
     * @brief Get results manager for direct access if needed
     */
    LiveResultsManager* get_results_manager() { return results_manager_.get(); }
    const LiveResultsManager* get_results_manager() const { return results_manager_.get(); }

    /**
     * @brief Get price manager for direct access if needed
     */
    LivePriceManager* get_price_manager() { return price_manager_.get(); }
    const LivePriceManager* get_price_manager() const { return price_manager_.get(); }

    /**
     * @brief Get PnL manager for direct access if needed
     */
    LivePnLManager* get_pnl_manager() { return pnl_manager_.get(); }
    const LivePnLManager* get_pnl_manager() const { return pnl_manager_.get(); }

    // ========== High-Level Operations ==========

    /**
     * @brief Initialize all components
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Load previous day's data for calculations
     * @param date Current date
     * @return Result with previous portfolio value and metrics
     */
    Result<std::pair<double, TradingMetrics>> load_previous_day_data(
        const Timestamp& date) const;

    /**
     * @brief Calculate all metrics for current day
     * @param daily_pnl Daily PnL
     * @param previous_portfolio_value Previous day's portfolio value
     * @param current_portfolio_value Current portfolio value
     * @param gross_notional Gross notional value
     * @param margin_posted Margin posted
     * @param trading_days Number of trading days
     * @param daily_commissions Daily commissions
     * @return Result with calculated metrics
     */
    Result<TradingMetrics> calculate_daily_metrics(
        double daily_pnl,
        double previous_portfolio_value,
        double current_portfolio_value,
        double gross_notional,
        double margin_posted,
        int trading_days,
        double daily_commissions = 0.0);

    /**
     * @brief Calculate metrics for previous day finalization
     * @param realized_pnl Realized PnL for the day
     * @param day_before_portfolio Previous day's portfolio value
     * @param current_portfolio Current portfolio value
     * @param gross_notional Gross notional value
     * @param margin_posted Margin posted
     * @param trading_days Number of trading days
     * @param commissions Commissions
     * @return Result with finalized metrics
     */
    Result<TradingMetrics> calculate_finalization_metrics(
        double realized_pnl,
        double day_before_portfolio,
        double current_portfolio,
        double gross_notional,
        double margin_posted,
        int trading_days,
        double commissions = 0.0);

    /**
     * @brief Store all results to database
     * @param metrics Metrics to store
     * @param positions Positions to store
     * @param date Current date
     * @return Result indicating success or failure
     */
    Result<void> store_results(
        const TradingMetrics& metrics,
        const std::vector<Position>& positions,
        const Timestamp& date);

    /**
     * @brief Get current metrics
     */
    const TradingMetrics& get_current_metrics() const { return current_metrics_; }

    /**
     * @brief Check if coordinator is initialized
     */
    bool is_initialized() const { return is_initialized_; }

    // ========== Convenience Methods ==========

    /**
     * @brief Load commissions for a specific date
     * @param date Date to load commissions for
     * @return Result with commission data by symbol
     */
    Result<std::unordered_map<std::string, double>> load_commissions_by_symbol(
        const Timestamp& date) const;

    /**
     * @brief Load positions for export
     * @param date Date to load positions for
     * @return Result with positions
     */
    Result<std::vector<Position>> load_positions_for_export(
        const Timestamp& date) const;

    /**
     * @brief Get trading days count
     * @return Result with number of trading days
     */
    Result<int> get_trading_days_count() const;

private:
    /**
     * @brief Validate database connection
     * @return Result indicating if connection is valid
     */
    Result<void> validate_connection() const;

    /**
     * @brief Convert CalculatedMetrics to TradingMetrics
     */
    TradingMetrics convert_calculated_metrics(
        const struct CalculatedMetrics& calc_metrics) const;
};

} // namespace trade_ngin