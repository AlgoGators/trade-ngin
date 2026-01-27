#pragma once

#include <memory>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
// Include component headers for unique_ptr (need complete types)
#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/backtest/backtest_price_manager.hpp"
#include "trade_ngin/backtest/backtest_pnl_manager.hpp"
#include "trade_ngin/backtest/backtest_execution_manager.hpp"
#include "trade_ngin/backtest/backtest_portfolio_constraints.hpp"
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

namespace trade_ngin {

// Forward declarations
class InstrumentRegistry;

namespace backtest {

/**
 * @brief Configuration for BacktestCoordinator
 */
struct BacktestCoordinatorConfig {
    double initial_capital = 1000000.0;
    bool use_risk_management = false;
    bool use_optimization = false;
    bool store_results = true;
    int warmup_days = 0;
    std::string results_schema = "backtest";
    bool store_trade_details = true;
    std::string portfolio_id = "BASE_PORTFOLIO";
};

/**
 * @brief Central orchestrator for backtest components
 *
 * This class mirrors LiveTradingCoordinator and manages the lifecycle
 * and coordination of all backtest components:
 * - BacktestDataLoader: Market data loading
 * - BacktestMetricsCalculator: Performance metric calculations
 * - BacktestPriceManager: Price history tracking
 * - BacktestPnLManager: PnL calculations
 * - BacktestExecutionManager: Execution generation
 * - BacktestPortfolioConstraints: Risk and optimization
 * - BacktestResultsManager: Storage operations
 *
 * It replaces the monolithic BacktestEngine by delegating to specialized
 * components while maintaining the same public API.
 */
class BacktestCoordinator {
private:
    // Configuration
    BacktestCoordinatorConfig config_;

    // Shared database connection
    std::shared_ptr<PostgresDatabase> db_;

    // Reference to instrument registry
    InstrumentRegistry* registry_;

    // Managed components
    std::unique_ptr<BacktestDataLoader> data_loader_;
    std::unique_ptr<BacktestMetricsCalculator> metrics_calculator_;
    std::unique_ptr<BacktestPriceManager> price_manager_;
    std::unique_ptr<BacktestPnLManager> pnl_manager_;
    std::unique_ptr<BacktestExecutionManager> execution_manager_;
    std::unique_ptr<BacktestPortfolioConstraints> constraints_manager_;

    // State for BOD model (replaces static variables)
    bool has_previous_bars_ = false;
    std::vector<Bar> previous_bars_;
    std::map<std::string, Position> current_positions_;
    double current_portfolio_value_ = 0.0;

    // Portfolio backtest state
    bool portfolio_has_previous_bars_ = false;
    std::vector<Bar> portfolio_previous_bars_;
    std::string current_run_id_;
    Timestamp backtest_start_date_;
    Timestamp backtest_end_date_;

    // Optional components for portfolio backtest
    std::shared_ptr<RiskManager> risk_manager_;

    // Initialization state
    bool is_initialized_ = false;

public:
    /**
     * @brief Constructor
     * @param db Shared database connection
     * @param registry Reference to instrument registry
     * @param config Configuration for the coordinator
     */
    BacktestCoordinator(
        std::shared_ptr<PostgresDatabase> db,
        InstrumentRegistry* registry,
        const BacktestCoordinatorConfig& config = {});

    /**
     * @brief Destructor
     */
    ~BacktestCoordinator();

    // ========== Component Access ==========

    BacktestDataLoader* get_data_loader() { return data_loader_.get(); }
    BacktestMetricsCalculator* get_metrics_calculator() { return metrics_calculator_.get(); }
    BacktestPriceManager* get_price_manager() { return price_manager_.get(); }
    BacktestPnLManager* get_pnl_manager() { return pnl_manager_.get(); }
    BacktestExecutionManager* get_execution_manager() { return execution_manager_.get(); }
    BacktestPortfolioConstraints* get_constraints_manager() { return constraints_manager_.get(); }

    // ========== High-Level Operations ==========

    /**
     * @brief Initialize all components
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Run backtest for a single strategy
     *
     * @param strategy Strategy to test
     * @param symbols Symbols to trade
     * @param start_date Start date for backtest
     * @param end_date End date for backtest
     * @param asset_class Asset class
     * @param data_freq Data frequency
     * @return BacktestResults or error
     */
    Result<BacktestResults> run_single_strategy(
        std::shared_ptr<StrategyInterface> strategy,
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class = AssetClass::FUTURES,
        DataFrequency data_freq = DataFrequency::DAILY);

    /**
     * @brief Run portfolio backtest with multiple strategies
     *
     * @param portfolio Portfolio manager with strategies
     * @param symbols Symbols to trade
     * @param start_date Start date for backtest
     * @param end_date End date for backtest
     * @param asset_class Asset class
     * @param data_freq Data frequency
     * @return BacktestResults or error
     */
    Result<BacktestResults> run_portfolio(
        std::shared_ptr<PortfolioManager> portfolio,
        const std::vector<std::string>& symbols,
        const Timestamp& start_date,
        const Timestamp& end_date,
        AssetClass asset_class = AssetClass::FUTURES,
        DataFrequency data_freq = DataFrequency::DAILY);

    /**
     * @brief Process a single day's data
     *
     * @param timestamp Current timestamp
     * @param bars Bars for this day
     * @param strategy Strategy to process
     * @param executions Output executions
     * @param equity_curve Output equity curve
     * @param risk_metrics Output risk metrics
     * @param is_warmup Whether this is a warmup period
     * @return Success or error
     */
    Result<void> process_day(
        const Timestamp& timestamp,
        const std::vector<Bar>& bars,
        std::shared_ptr<StrategyInterface> strategy,
        std::vector<ExecutionReport>& executions,
        std::vector<std::pair<Timestamp, double>>& equity_curve,
        std::vector<RiskResult>& risk_metrics,
        bool is_warmup = false);

    /**
     * @brief Reset state for new backtest run
     */
    void reset();

    /**
     * @brief Check if coordinator is initialized
     */
    bool is_initialized() const { return is_initialized_; }

    /**
     * @brief Get current positions
     */
    const std::map<std::string, Position>& get_current_positions() const {
        return current_positions_;
    }

    /**
     * @brief Get current portfolio value
     */
    double get_current_portfolio_value() const {
        return current_portfolio_value_;
    }

    // ========== Results Storage ==========

    /**
     * @brief Save portfolio backtest results to database
     *
     * @param results Backtest results to save
     * @param strategy_names Names of strategies in the portfolio
     * @param strategy_allocations Allocation percentage for each strategy
     * @param portfolio Portfolio manager
     * @param portfolio_config Portfolio configuration as JSON
     * @return Result indicating success or failure
     */
    Result<void> save_portfolio_results_to_db(
        const BacktestResults& results,
        const std::vector<std::string>& strategy_names,
        const std::unordered_map<std::string, double>& strategy_allocations,
        std::shared_ptr<PortfolioManager> portfolio,
        const nlohmann::json& portfolio_config) const;

private:
    /**
     * @brief Create and initialize components
     */
    Result<void> create_components();

    /**
     * @brief Validate database connection
     */
    Result<void> validate_connection() const;

    /**
     * @brief Calculate portfolio value from positions
     */
    double calculate_portfolio_value(
        const std::map<std::string, Position>& positions,
        const std::vector<Bar>& bars);

    // ========== Portfolio Backtest Helpers ==========

    /**
     * @brief Process a single day's data for portfolio backtest
     */
    Result<void> process_portfolio_day(
        const Timestamp& timestamp,
        const std::vector<Bar>& bars,
        std::shared_ptr<PortfolioManager> portfolio,
        std::vector<ExecutionReport>& executions,
        std::vector<std::pair<Timestamp, double>>& equity_curve,
        std::vector<RiskResult>& risk_metrics,
        bool is_warmup,
        double initial_capital);

    /**
     * @brief Calculate warmup days from strategy lookbacks
     */
    static int calculate_warmup_days(
        const std::vector<std::shared_ptr<StrategyInterface>>& strategies);

    /**
     * @brief Generate run_id for portfolio backtest
     */
    std::string generate_portfolio_run_id(
        const std::vector<std::string>& strategy_names,
        const Timestamp& end_date);

    /**
     * @brief Reset portfolio-specific state
     */
    void reset_portfolio_state();

    /**
     * @brief Save daily positions to database
     */
    Result<void> save_daily_positions(
        std::shared_ptr<PortfolioManager> portfolio,
        const std::string& run_id,
        const Timestamp& timestamp);

    /**
     * @brief Calculate transaction costs from new executions in period
     */
    double calculate_period_transaction_costs(
        std::shared_ptr<PortfolioManager> portfolio,
        const std::unordered_map<std::string, size_t>& exec_counts_before);

    /**
     * @brief Find bar for a given symbol
     */
    std::optional<Bar> find_bar_for_symbol(
        const std::vector<Bar>& bars,
        const std::string& symbol);
};

} // namespace backtest
} // namespace trade_ngin
