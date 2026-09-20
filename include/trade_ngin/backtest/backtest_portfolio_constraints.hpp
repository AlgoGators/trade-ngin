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
    bool use_optimization = false;
    size_t max_history_length = 252;  // Max periods for covariance calculation
    size_t min_periods_for_covariance = 20;  // Minimum periods needed for covariance
    double default_variance = 0.01;  // Default variance for diagonal fallback
};

/**
 * @brief Apply portfolio optimization constraints
 *
 * This class extracts the apply_portfolio_constraints() method and related
 * helper methods from BacktestEngine (lines 1268-1416, 3164-3300).
 *
 * Key responsibilities:
 * - Apply portfolio optimization
 * - Track historical returns for covariance calculation
 * - Calculate covariance matrix for optimization
 *
 * Dependencies:
 * - DynamicOptimizer (existing component)
 *
 * The risk gate this class used to carry (a RiskManager set only by a test, so it never
 * scaled a position in any runner) was deleted; the portfolio's risk management runs in
 * PortfolioManager::apply_risk_management.
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
        std::shared_ptr<DynamicOptimizer> optimizer);

    ~BacktestPortfolioConstraints() = default;

    /**
     * @brief Apply all portfolio constraints
     *
     * Applies optimization. Modifies positions in-place.
     *
     * @param bars Not read since the risk gate was deleted
     * @param current_positions Positions to constrain (modified in-place)
     * @param risk_metrics Not written since the risk gate was deleted
     *
     * bars and risk_metrics stay in the signature so the single-strategy caller
     * (BacktestCoordinator::process_day) is unchanged.
     * @return Success or error
     */
    Result<void> apply_constraints(
        const std::vector<Bar>& bars,
        std::map<std::string, Position>& current_positions,
        std::vector<RiskResult>& risk_metrics);

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
     * @brief Check if optimization is enabled and available
     */
    bool is_optimization_enabled() const {
        return config_.use_optimization && optimizer_ != nullptr;
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
