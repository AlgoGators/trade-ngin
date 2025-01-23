//include/trade_ngin/risk/risk_manager.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <memory>
#include <unordered_map>
#include <map>

namespace trade_ngin {

/**
 * @brief Configuration for risk management
 */
struct RiskConfig {
    // Portfolio risk parameters
    double portfolio_var_limit{0.15};        // Value at Risk limit
    double max_drawdown{0.20};               // Maximum allowed drawdown
    
    // Jump risk parameters
    double jump_risk_threshold{0.10};        // Threshold for jump risk
    bool use_historical_jumps{true};         // Use historical jumps vs parametric
    
    // Correlation risk parameters
    double max_correlation{0.7};             // Maximum allowed correlation
    double correlation_lookback{252};        // Days for correlation calculation
    
    // Leverage risk parameters
    double max_gross_leverage{4.0};          // Maximum gross leverage
    double max_net_leverage{2.0};            // Maximum net leverage
    
    // General parameters
    double capital{0.0};                     // Total portfolio capital
    double confidence_level{0.99};           // Confidence level for risk calcs
    int lookback_period{252};               // Historical lookback period
};

/**
 * @brief Result from risk calculations
 */
struct RiskResult {
    bool risk_exceeded{false};              // Whether any risk limit was exceeded
    double recommended_scale{1.0};          // Recommended position scaling

    // Individual risk metrics
    double portfolio_risk{0.0};           
    double jump_risk{0.0};
    double correlation_risk{0.0};
    double leverage_risk{0.0};

    // Individual multipliers
    double portfolio_multiplier{1.0};
    double jump_multiplier{1.0};
    double correlation_multiplier{1.0};
    double leverage_multiplier{1.0};

    // Historical metrics
    double max_portfolio_risk{0.0};
    double max_jump_risk{0.0};
    double max_correlation_risk{0.0};
    double max_leverage{0.0};
};

/**
 * @brief Manager for multiple types of portfolio risk
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
     * @brief Calculate portfolio risk multiplier
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return Portfolio risk multiplier between 0 and 1
     */
    double calculate_portfolio_multiplier(
        const std::unordered_map<std::string, Position>& positions,
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate jump risk multiplier
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return Jump risk multiplier between 0 and 1
     */
    double calculate_jump_multiplier(
        const std::unordered_map<std::string, Position>& positions,
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate correlation risk multiplier
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return Correlation risk multiplier between 0 and 1
     */
    double calculate_correlation_multiplier(
        const std::unordered_map<std::string, Position>& positions,
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate leverage risk multiplier
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return Leverage risk multiplier between 0 and 1
     */
    double calculate_leverage_multiplier(
        const std::unordered_map<std::string, Position>& positions,
        const std::vector<std::vector<double>>& returns) const;

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
     * @brief Calculate historical returns
     * @param data Market data bars
     * @return Matrix of returns per symbol
     */
    std::vector<std::vector<double>> calculate_returns(
        const std::vector<Bar>& data) const;

    /**
     * @brief Calculate covariance matrix
     * @param returns Return matrix
     * @return Covariance matrix
     */
    std::vector<std::vector<double>> calculate_covariance(
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate Value at Risk
     * @param positions Portfolio positions
     * @param returns Historical returns
     * @return VaR at configured confidence level
     */
    double calculate_var(
        const std::unordered_map<std::string, Position>& positions,
        const std::vector<std::vector<double>>& returns) const;

    /**
     * @brief Calculate position weights
     * @param positions Portfolio positions
     * @return Vector of position weights
     */
    std::vector<double> calculate_weights(
        const std::unordered_map<std::string, Position>& positions) const;
};

} // namespace trade_ngin