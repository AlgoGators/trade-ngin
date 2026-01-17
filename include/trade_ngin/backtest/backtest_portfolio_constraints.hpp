#pragma once

#include <memory>
#include <map>
#include <vector>
#include <unordered_map>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief Configuration for portfolio constraints
 */
struct PortfolioConstraintsConfig {
    bool use_risk_management = false;
    bool use_optimization = false;
    double commission_rate = 0.0005;
    size_t max_history_length = 252;  // Max periods for covariance calculation
    size_t min_periods_for_covariance = 20;  // Minimum periods needed for covariance
    double default_variance = 0.01;  // Default variance for diagonal fallback
};

/**
 * @brief Apply risk management and portfolio optimization constraints
 *
 * This class extracts the apply_portfolio_constraints() method and related
 * helper methods from BacktestEngine (lines 1268-1416, 3164-3300).
 *
 * Key responsibilities:
 * - Apply risk management constraints (scale positions if risk exceeded)
 * - Apply portfolio optimization
 * - Track historical returns for covariance calculation
 * - Calculate covariance matrix for optimization
 *
 * Dependencies:
 * - RiskManager (existing component)
 * - DynamicOptimizer (existing component)
 */
class BacktestPortfolioConstraints {
public:
    /**
     * @brief Constructor with config only
     */
    explicit BacktestPortfolioConstraints(const PortfolioConstraintsConfig& config);

    /**
     * @brief Constructor with full dependencies
     */
    BacktestPortfolioConstraints(
        const PortfolioConstraintsConfig& config,
        std::shared_ptr<RiskManager> risk_manager,
        std::shared_ptr<DynamicOptimizer> optimizer);

    ~BacktestPortfolioConstraints() = default;

    /**
     * @brief Apply all portfolio constraints
     *
     * Applies risk management and optimization in sequence.
     * Modifies positions in-place.
     *
     * @param bars Current market data bars
     * @param current_positions Positions to constrain (modified in-place)
     * @param risk_metrics Output vector for risk metric history
     * @return Success or error
     */
    Result<void> apply_constraints(
        const std::vector<Bar>& bars,
        std::map<std::string, Position>& current_positions,
        std::vector<RiskResult>& risk_metrics);

    /**
     * @brief Apply risk management only
     *
     * @param bars Current market data bars
     * @param current_positions Positions to check
     * @return RiskResult with scaling recommendation
     */
    Result<RiskResult> apply_risk_management(
        const std::vector<Bar>& bars,
        const std::map<std::string, Position>& positions);

    /**
     * @brief Apply optimization only
     *
     * @param current_positions Positions to optimize (modified in-place)
     * @return Success or error
     */
    Result<void> apply_optimization(
        std::map<std::string, Position>& current_positions);

    /**
     * @brief Update historical returns from new bars
     *
     * Call this for each day's bars to maintain price/return history
     * for covariance calculation.
     *
     * @param bars Market data bars
     */
    void update_historical_returns(const std::vector<Bar>& bars);

    /**
     * @brief Calculate covariance matrix
     *
     * @param symbols Symbols to include (in desired order)
     * @return NxN covariance matrix
     */
    std::vector<std::vector<double>> calculate_covariance_matrix(
        const std::vector<std::string>& symbols) const;

    /**
     * @brief Check if risk management is enabled and available
     */
    bool is_risk_management_enabled() const {
        return config_.use_risk_management && risk_manager_ != nullptr;
    }

    /**
     * @brief Check if optimization is enabled and available
     */
    bool is_optimization_enabled() const {
        return config_.use_optimization && optimizer_ != nullptr;
    }

    /**
     * @brief Set risk manager
     */
    void set_risk_manager(std::shared_ptr<RiskManager> risk_manager) {
        risk_manager_ = std::move(risk_manager);
    }

    /**
     * @brief Set optimizer
     */
    void set_optimizer(std::shared_ptr<DynamicOptimizer> optimizer) {
        optimizer_ = std::move(optimizer);
    }

    /**
     * @brief Reset all historical data
     */
    void reset();

    /**
     * @brief Get returns history length for a symbol
     */
    size_t get_history_length(const std::string& symbol) const;

private:
    PortfolioConstraintsConfig config_;
    std::shared_ptr<RiskManager> risk_manager_;
    std::shared_ptr<DynamicOptimizer> optimizer_;

    // Historical data for covariance calculation
    std::unordered_map<std::string, std::vector<double>> price_history_;
    std::unordered_map<std::string, std::vector<double>> historical_returns_;

    /**
     * @brief Get returns for covariance calculation
     */
    std::unordered_map<std::string, std::vector<double>> get_returns_for_symbols(
        const std::vector<std::string>& symbols) const;
};

} // namespace backtest
} // namespace trade_ngin
