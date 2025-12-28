// include/trade_ngin/portfolio/portfolio_manager.hpp
#pragma once

#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <numeric>
#include <unordered_map>
#include "trade_ngin/backtest/slippage_models.hpp"
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace trade_ngin {

/**
 * @brief Configuration for portfolio management
 */
struct PortfolioConfig : public ConfigBase {
    Decimal total_capital{Decimal(0.0)};    // Total portfolio capital
    Decimal reserve_capital{Decimal(0.0)};  // Capital to keep in reserve
    double max_strategy_allocation{
        1.0};  // Maximum allocation to any strategy (keep as double - it's a ratio)
    double min_strategy_allocation{
        0.0};  // Minimum allocation to any strategy (keep as double - it's a ratio)
    bool use_optimization{false};     // Whether to use position optimization
    bool use_risk_management{false};  // Whether to use risk management
    DynamicOptConfig opt_config;      // Optimization configuration
    RiskConfig risk_config;           // Risk management configuration

    std::string version{"1.0.0"};  // Configuration version

    PortfolioConfig() = default;

    PortfolioConfig(Decimal total_capital, Decimal reserve_capital, double max_strategy_allocation,
                    double min_strategy_allocation, bool use_optimization, bool use_risk_management)
        : total_capital(total_capital),
          reserve_capital(reserve_capital),
          max_strategy_allocation(max_strategy_allocation),
          min_strategy_allocation(min_strategy_allocation),
          use_optimization(use_optimization),
          use_risk_management(use_risk_management) {}

    // JSON serialization
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["total_capital"] = static_cast<double>(total_capital);
        j["reserve_capital"] = static_cast<double>(reserve_capital);
        j["max_strategy_allocation"] = max_strategy_allocation;
        j["min_strategy_allocation"] = min_strategy_allocation;
        j["use_optimization"] = use_optimization;
        j["use_risk_management"] = use_risk_management;
        j["opt_config"] = opt_config.to_json();
        j["risk_config"] = risk_config.to_json();
        j["version"] = version;
        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("total_capital"))
            total_capital = Decimal(j.at("total_capital").get<double>());
        if (j.contains("reserve_capital"))
            reserve_capital = Decimal(j.at("reserve_capital").get<double>());
        if (j.contains("max_strategy_allocation")) {
            max_strategy_allocation = j.at("max_strategy_allocation").get<double>();
        }
        if (j.contains("min_strategy_allocation")) {
            min_strategy_allocation = j.at("min_strategy_allocation").get<double>();
        }
        if (j.contains("use_optimization")) {
            use_optimization = j.at("use_optimization").get<bool>();
        }
        if (j.contains("use_risk_management")) {
            use_risk_management = j.at("use_risk_management").get<bool>();
        }
        if (j.contains("opt_config"))
            opt_config.from_json(j.at("opt_config"));
        if (j.contains("risk_config"))
            risk_config.from_json(j.at("risk_config"));
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Manages multiple strategies and their allocations
 * Optionally applies optimization and risk management
 */
class PortfolioManager {
public:
    /**
     * @brief Constructor
     * @param config Portfolio configuration
     * @param id Optional identifier for this manager
     */
    explicit PortfolioManager(PortfolioConfig config, std::string id = "PORTFOLIO_MANAGER",
                              std::shared_ptr<InstrumentRegistry> registry = nullptr);

    /**
     * @brief Add a strategy to the portfolio
     * @param strategy Strategy to add
     * @param initial_allocation Initial capital allocation
     * @param use_optimization Whether this strategy uses optimization
     * @param use_risk_management Whether this strategy uses risk management
     * @return Result indicating success or failure
     */
    Result<void> add_strategy(std::shared_ptr<StrategyInterface> strategy,
                              double initial_allocation, bool use_optimization = false,
                              bool use_risk_management = false);

    /**
     * @brief Process new market data
     * @param data New market data
     * @param skip_execution_generation If true, skip execution generation (used during warmup)
     * @param current_timestamp Optional current day's timestamp for execution fill_time (if not provided, uses data[0].timestamp)
     * @return Result indicating success or failure
     */
    Result<void> process_market_data(const std::vector<Bar>& data, bool skip_execution_generation = false, 
                                     std::optional<Timestamp> current_timestamp = std::nullopt);

    /**
     * @brief Update strategy allocations
     * @param allocations Map of strategy ID to allocation
     * @return Result indicating success or failure
     */
    Result<void> update_allocations(const std::unordered_map<std::string, double>& allocations);

    /**
     * @brief Get current portfolio positions
     * @return Map of symbol to aggregated position
     */
    std::unordered_map<std::string, Position> get_portfolio_positions() const;

    /**
     * @brief Get position changes needed
     * @return Map of symbol to required position change
     */
    std::unordered_map<std::string, double> get_required_changes() const;

    /**
     * @brief Get recent execution reports from position changes
     * @return Vector of execution reports since last call
     */
    std::vector<ExecutionReport> get_recent_executions() const;

    /**
     * @brief Get recent execution reports per strategy
     * @return Map of strategy ID to vector of execution reports
     */
    std::unordered_map<std::string, std::vector<ExecutionReport>> get_strategy_executions() const;

    /**
     * @brief Clear the execution history (useful after retrieving them)
     */
    void clear_execution_history();

    /**
     * @brief Clear all executions including strategy-level (used during warmup)
     */
    void clear_all_executions();

    /**
     * @brief Get all strategies managed by this portfolio
     * @return Vector of strategy interfaces
     */
    std::vector<std::shared_ptr<StrategyInterface>> get_strategies() const;

    /**
     * @brief Get optimized positions per strategy (after optimization/rounding)
     * @return Map of strategy ID to map of symbol to position
     */
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> get_strategy_positions() const;

    /**
     * @brief Update a specific position for a strategy (e.g., to update PnL values)
     * @param strategy_id Strategy identifier
     * @param symbol Position symbol
     * @param updated_pos Updated position object
     * @return Result indicating success or failure
     */
    Result<void> update_strategy_position(
        const std::string& strategy_id,
        const std::string& symbol,
        const Position& updated_pos);

    /**
     * @brief Get portfolio's current total value
     * @param current_prices Map of symbol to current price
     * @return Current portfolio value including cash
     */
    double get_portfolio_value(const std::unordered_map<std::string, double>& current_prices) const;

    /**
     * @brief Get the portfolio's configuration
     * @return Portfolio configuration
     */
    const PortfolioConfig& get_config() const {
        return config_;
    }

    /**
     * @brief Set external risk manager to use instead of internal one
     * @param manager Shared pointer to an existing risk manager
     */
    void set_risk_manager(std::shared_ptr<RiskManager> manager) {
        if (manager) {
            // Store the provided manager
            external_risk_manager_ = manager;
            // Disable the internal manager
            risk_manager_.reset();
        }
    }

private:
    PortfolioConfig config_;
    std::string id_;

    std::unique_ptr<DynamicOptimizer> optimizer_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::shared_ptr<RiskManager> external_risk_manager_{nullptr};
    std::shared_ptr<InstrumentRegistry> registry_{nullptr};

    struct StrategyInfo {
        std::shared_ptr<StrategyInterface> strategy;
        double allocation;
        bool use_optimization;
        bool use_risk_management;
        std::unordered_map<std::string, Position> current_positions;
        std::unordered_map<std::string, Position> target_positions;
    };

    std::unordered_map<std::string, StrategyInfo> strategies_;
    std::vector<ExecutionReport> recent_executions_;  // Portfolio-level (aggregated)
    std::unordered_map<std::string, std::vector<ExecutionReport>> strategy_executions_;  // Per-strategy executions
    
    // Track previous day close prices for PnL Lag Model (prevents lookahead bias)
    std::unordered_map<std::string, double> previous_day_close_prices_;

    mutable std::mutex mutex_;
    const std::string instance_id_;

    std::unordered_map<std::string, std::vector<double>> price_history_;
    std::unordered_map<std::string, std::vector<double>> historical_returns_;
    std::vector<Bar> risk_history_;
    MarketData current_market_data_;

    size_t max_history_length_ = 2520;  // Keep up to 1 year of return data

    /**
     * @brief Calculate weights per contract for each symbol
     * @param symbols List of symbols to calculate weights for
     * @param capital Total capital available for allocation
     * @return Vector of weights per contract for each symbol
     */
    std::vector<double> calculate_weights_per_contract(const std::vector<std::string>& symbols,
                                                       double capital) const;

    /**
     * @brief Calculate trading costs for each symbol
     * @param symbols List of symbols to calculate costs for
     * @param capital Total capital available for allocation
     * @return Vector of trading costs for each symbol
     */
    std::vector<double> calculate_trading_costs(const std::vector<std::string>& symbols,
                                                double capital) const;

    /**
     * @brief Update historical returns for all symbols
     * @param data New market data
     */
    void update_historical_returns(const std::vector<Bar>& data);

    /**
     * @brief Calculate covariance matrix from returns
     * @param returns_by_symbol Map of symbol to returns
     * @return Covariance matrix
     */
    std::vector<std::vector<double>> calculate_covariance_matrix(
        const std::unordered_map<std::string, std::vector<double>>& returns_by_symbol);

    /**
     * @brief Optimize positions for strategies that use optimization
     * @return Result indicating success or failure
     */
    Result<void> optimize_positions();

    /**
     * @brief Apply risk management to positions
     * @return Result indicating success or failure
     */
    Result<void> apply_risk_management(const std::vector<Bar>& data);

    /**
     * @brief Validate allocations sum to 1
     * @param allocations Strategy allocations
     * @return Result indicating if allocations are valid
     */
    Result<void> validate_allocations(
        const std::unordered_map<std::string, double>& allocations) const;

    /**
     * @brief Get positions from all strategies
     * @return Map of symbol to position across all strategies
     */
    std::unordered_map<std::string, Position> get_positions_internal() const;
};

}  // namespace trade_ngin