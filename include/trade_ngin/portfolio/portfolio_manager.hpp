// include/trade_ngin/portfolio/portfolio_manager.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <numeric>
#include <iostream>

namespace trade_ngin {

/**
 * @brief Configuration for portfolio management
 */
struct PortfolioConfig : public ConfigBase {
    double total_capital{0.0};            // Total portfolio capital
    double reserve_capital{0.0};          // Capital to keep in reserve
    double max_strategy_allocation{1.0};   // Maximum allocation to any strategy
    double min_strategy_allocation{0.0};   // Minimum allocation to any strategy
    bool use_optimization{false};         // Whether to use position optimization
    bool use_risk_management{false};      // Whether to use risk management
    DynamicOptConfig opt_config;          // Optimization configuration
    RiskConfig risk_config;               // Risk management configuration

    std::string version{"1.0.0"};         // Configuration version

    PortfolioConfig() = default;

    PortfolioConfig(double total_capital, double reserve_capital,
        double max_strategy_allocation, double min_strategy_allocation,
        bool use_optimization, bool use_risk_management)
        : total_capital(total_capital), reserve_capital(reserve_capital),
          max_strategy_allocation(max_strategy_allocation),
          min_strategy_allocation(min_strategy_allocation),
          use_optimization(use_optimization),
          use_risk_management(use_risk_management) {}

    // JSON serialization
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["total_capital"] = total_capital;
        j["reserve_capital"] = reserve_capital;
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
        if (j.contains("total_capital")) total_capital = j.at("total_capital").get<double>();
        if (j.contains("reserve_capital")) reserve_capital = j.at("reserve_capital").get<double>();
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
        if (j.contains("opt_config")) opt_config.from_json(j.at("opt_config"));
        if (j.contains("risk_config")) risk_config.from_json(j.at("risk_config"));
        if (j.contains("version")) version = j.at("version").get<std::string>();
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
    explicit PortfolioManager(PortfolioConfig config, std::string id = "PORTFOLIO_MANAGER");

    /**
     * @brief Add a strategy to the portfolio
     * @param strategy Strategy to add
     * @param initial_allocation Initial capital allocation
     * @param use_optimization Whether this strategy uses optimization
     * @param use_risk_management Whether this strategy uses risk management
     * @return Result indicating success or failure
     */
    Result<void> add_strategy(
        std::shared_ptr<StrategyInterface> strategy,
        double initial_allocation,
        bool use_optimization = false,
        bool use_risk_management = false
    );

    /**
     * @brief Process new market data
     * @param data New market data
     * @return Result indicating success or failure
     */
    Result<void> process_market_data(const std::vector<Bar>& data);

    /**
     * @brief Update strategy allocations
     * @param allocations Map of strategy ID to allocation
     * @return Result indicating success or failure
     */
    Result<void> update_allocations(
        const std::unordered_map<std::string, double>& allocations
    );

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
     * @brief Clear the execution history (useful after retrieving them)
     */
    void clear_execution_history();
    
    /**
     * @brief Get all strategies managed by this portfolio
     * @return Vector of strategy interfaces
     */
    std::vector<std::shared_ptr<StrategyInterface>> get_strategies() const;
    
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
    const PortfolioConfig& get_config() const { return config_; }

    /**
     * @brief Set external risk manager to use instead of internal one
     * @param manager Pointer to an existing risk manager
     */
    void set_risk_manager(RiskManager* manager) {
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
    RiskManager* external_risk_manager_{nullptr};

    struct StrategyInfo {
        std::shared_ptr<StrategyInterface> strategy;
        double allocation;
        bool use_optimization;
        bool use_risk_management;
        std::unordered_map<std::string, Position> current_positions;
        std::unordered_map<std::string, Position> target_positions;
    };

    std::unordered_map<std::string, StrategyInfo> strategies_;
    std::vector<ExecutionReport> recent_executions_;
    mutable std::mutex mutex_;
    const std::string instance_id_;

    /**
     * @brief Optimize positions for strategies that use optimization
     * @return Result indicating success or failure
     */
    Result<void> optimize_positions();

    /**
     * @brief Apply risk management to positions
     * @return Result indicating success or failure
     */
    Result<void> apply_risk_management();

    /**
     * @brief Validate allocations sum to 1
     * @param allocations Strategy allocations
     * @return Result indicating if allocations are valid
     */
    Result<void> validate_allocations(
        const std::unordered_map<std::string, double>& allocations
    ) const;

    /**
     * @brief Get positions from all strategies
     * @return Map of symbol to position across all strategies
     */
    std::unordered_map<std::string, Position> get_positions_internal() const;
};

} // namespace trade_ngin