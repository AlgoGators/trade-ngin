#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>
#include <iostream>

namespace trade_ngin {

/**
 * @brief Configuration for risk management
 */
struct RiskConfig {
    // Risk limits
    double var_limit{0.15};          // Value at Risk limit (15%)
    double jump_risk_limit{0.10};    // Jump risk threshold (10%)
    double max_correlation{0.7};     // Maximum allowed correlation
    double max_gross_leverage{4.0};  // Maximum gross leverage
    double max_net_leverage{2.0};    // Maximum net leverage
    
    // Calculation parameters
    double confidence_level{0.99};   // Confidence level for risk calcs
    int lookback_period{252};        // Historical lookback period
    double capital{1e6};             // Portfolio capital
};

/**
 * @brief Result from risk calculations
 */
struct RiskResult {
    bool risk_exceeded{false};      // Flag indicating risk exceeded
    double recommended_scale{1.0};  // Recommended scale-down factor
    
    // Risk metrics             
    double portfolio_var{0.0};      // Portfolio Value at Risk
    double jump_risk{0.0};          // Jump risk
    double correlation_risk{0.0};   // Correlation risk
    double gross_leverage{0.0};     // Gross leverage
    double net_leverage{0.0};       // Net leverage
    
    // Maximum observed risks
    double max_portfolio_risk{0.0};  // Maximum portfolio risk
    double max_jump_risk{0.0};      // Maximum jump risk
    double max_leverage_risk{0.0};   // Maximum leverage risk

    // Individual multipliers
    double portfolio_multiplier{1.0};     // Portfolio VaR multiplier
    double jump_multiplier{1.0};          // Jump risk multiplier
    double correlation_multiplier{1.0};    // Correlation multiplier
    double leverage_multiplier{1.0};      // Leverage multiplier
};

/**
 * @brief Risk management class
 */
class RiskManager {
public:
    explicit RiskManager(RiskConfig config);

    /**
     * @brief Process positions and calculate risk metrics
     * @param positions Current portfolio positions
     * @return Result containing risk calculations
     */
    Result<RiskResult> process_positions(
        const std::unordered_map<std::string, Position>& positions);

    /**
     * @brief Update market data for risk calculations
     * @param data New market data
     * @return Result indicating success or failure
     */
    Result<void> update_market_data(const std::vector<Bar>& data);

    /**
     * @brief Update risk configuration
     * @param config New configuration
     * @return Result indicating success or failure
     */
    Result<void> update_config(const RiskConfig& config);

    /**
     * @brief Get current risk configuration
     * @return Current configuration
     */
    const RiskConfig& get_config() const { return config_; }

private:
    RiskConfig config_;
    
    // Market data storage
    struct MarketData {
        std::vector<std::vector<double>> returns;
        std::vector<std::vector<double>> covariance;
        std::unordered_map<std::string, size_t> symbol_indices;
    };
    MarketData market_data_;

    /**
     * @brief Calculate position weights
     * @param positions Portfolio positions
     * @return Vector of position weights
     */
    std::vector<double> calculate_weights(
        const std::unordered_map<std::string, Position>& positions) const;

    /**
     * @brief Calculate the portfolio multiplier based on position weights
     * @param weights Vector of position weights
     * @param result RiskResult object to store intermediate calculations
     * @return Portfolio multiplier
     */
    double calculate_portfolio_multiplier(
        const std::vector<double>& weights,
        RiskResult& result) const;

    /**
     * @brief Calculate the jump multiplier based on position weights
     * @param weights Vector of position weights
     * @param result RiskResult object to store intermediate calculations
     * @return Jump multiplier
     */
    double calculate_jump_multiplier(
        const std::vector<double>& weights,
        RiskResult& result) const;
    
    /**
     * @brief Calculate the correlation multiplier based on position weights
     * @param weights Vector of position weights
     * @param result RiskResult object to store intermediate calculations
     * @return Correlation multiplier
     */
    double calculate_correlation_multiplier(
        const std::vector<double>& weights,
        RiskResult& result) const;

    /**
     * @brief Calculate the leverage multiplier based on position weights
     * @param weights Vector of position weights
     * @param total_value Total portfolio value
     * @param result RiskResult object to store intermediate calculations
     * @return Leverage multiplier
     */
    double calculate_leverage_multiplier(
        const std::vector<double>& weights,
        double total_value,
        RiskResult& result) const;

    /**
     * @brief Calculate historical returns from market data
     * @param data Market data bars
     * @return Matrix of returns
     */
    std::vector<std::vector<double>> calculate_returns(
        const std::vector<Bar>& data) const;

    /**
     * @brief Calculate covariance matrix from returns
     * @param returns Return matrix
     * @return Covariance matrix
     */
    std::vector<std::vector<double>> calculate_covariance(
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate Value at Risk for portfolio
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return Calculated VaR
     */
    double calculate_var(
        const std::unordered_map<std::string, Position>& positions,
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate the nth percentile of given data
     * @param data Vector of values
     * @return Calculated percentile value
     */
    double calculate_99th_percentile(
        const std::vector<double>& data) const;
};

} // namespace trade_ngin