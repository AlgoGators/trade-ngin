// include/trade_ngin/live/live_trading_coordinator.hpp
// Coordinator for all live trading components - ensures proper initialization and connection
// management

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
    // Storage strategy_name. EMPTY means "same as strategy_id", which is what the
    // futures runners have always relied on -- their rows are keyed
    // (strategy_id, strategy_id, portfolio_id). Set it only when the read key uses a
    // distinct name, as the equity runner does (reads EQUITY_MEAN_REVERSION, writes
    // under LIVE_EQUITY_MEAN_REVERSION unless this is populated).
    std::string strategy_name;
    std::string portfolio_id = "BASE_PORTFOLIO";
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
    double gross_leverage = 0.0;
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
    bool is_initialized_ = false;

public:
    /**
     * @brief Constructor
     * @param db Shared database connection
     * @param registry Reference to instrument registry
     * @param config Configuration for the coordinator
     */
    LiveTradingCoordinator(std::shared_ptr<PostgresDatabase> db, InstrumentRegistry& registry,
                           const LiveTradingConfig& config = {});

    /**
     * @brief Destructor
     */
    ~LiveTradingCoordinator();

    // ========== Component Access ==========

    /**
     * @brief Get data loader for direct access if needed
     */
    LiveDataLoader* get_data_loader() {
        return data_loader_.get();
    }
    const LiveDataLoader* get_data_loader() const {
        return data_loader_.get();
    }

    /**
     * @brief Get metrics calculator for direct access if needed
     */
    LiveMetricsCalculator* get_metrics_calculator() {
        return metrics_calculator_.get();
    }
    const LiveMetricsCalculator* get_metrics_calculator() const {
        return metrics_calculator_.get();
    }

    /**
     * @brief Get results manager for direct access if needed
     */
    LiveResultsManager* get_results_manager() {
        return results_manager_.get();
    }
    const LiveResultsManager* get_results_manager() const {
        return results_manager_.get();
    }

    /**
     * @brief Get price manager for direct access if needed
     */
    LivePriceManager* get_price_manager() {
        return price_manager_.get();
    }
    const LivePriceManager* get_price_manager() const {
        return price_manager_.get();
    }

    /**
     * @brief Get PnL manager for direct access if needed
     */
    LivePnLManager* get_pnl_manager() {
        return pnl_manager_.get();
    }
    const LivePnLManager* get_pnl_manager() const {
        return pnl_manager_.get();
    }

    // ========== High-Level Operations ==========

    /**
     * @brief Initialize all components
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Store all results to database
     * @param metrics Metrics to store
     * @param positions Positions to store
     * @param date Current date
     * @return Result indicating success or failure
     */
    Result<void> store_results(const TradingMetrics& metrics,
                               const std::vector<Position>& positions, const Timestamp& date);

    /**
     * @brief Check if coordinator is initialized
     */
    bool is_initialized() const {
        return is_initialized_;
    }

    // ========== Convenience Methods ==========

    // Note: this coordinator's load_commissions_by_symbol wrapper was deleted as dead code.
    // LiveDataLoader::load_commissions_by_symbol itself was NOT deleted -- it is live and
    // called by the equity runner every session (drift-D: the previous wording said "the
    // deleted LiveDataLoader equivalent", which is false).

    /**
     * @brief Load positions for export
     * @param date Date to load positions for
     * @return Result with positions
     */
    Result<std::vector<Position>> load_positions_for_export(const Timestamp& date) const;

private:
    /**
     * @brief Validate database connection
     * @return Result indicating if connection is valid
     */
    Result<void> validate_connection() const;
};

}  // namespace trade_ngin