// include/trade_ngin/portfolio/portfolio_manager.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include <memory>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief Configuration for portfolio management
 */
struct PortfolioConfig {
    double total_capital{0.0};            // Total portfolio capital
    double reserve_capital{0.0};          // Capital to keep in reserve
    double max_strategy_allocation{1.0};   // Maximum allocation to any strategy
    double min_strategy_allocation{0.0};   // Minimum allocation to any strategy
    bool use_optimization{false};         // Whether to use position optimization
    bool use_risk_management{false};      // Whether to use risk management
    DynamicOptConfig opt_config;          // Optimization configuration
    RiskConfig risk_config;               // Risk management configuration
};

/**
 * @brief Manages multiple strategies and their allocations
 * Optionally applies optimization and risk management
 */
class PortfolioManager {
public:
    explicit PortfolioManager(PortfolioConfig config);

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

private:
    PortfolioConfig config_;
    std::unique_ptr<DynamicOptimizer> optimizer_;
    std::unique_ptr<RiskManager> risk_manager_;

    struct StrategyInfo {
        std::shared_ptr<StrategyInterface> strategy;
        double allocation;
        bool use_optimization;
        bool use_risk_management;
        std::unordered_map<std::string, Position> current_positions;
        std::unordered_map<std::string, Position> target_positions;
    };

    std::unordered_map<std::string, StrategyInfo> strategies_;
    mutable std::mutex mutex_;

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
};

} // namespace trade_ngin