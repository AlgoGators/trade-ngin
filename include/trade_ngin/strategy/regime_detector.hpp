// include/trade_ngin/strategy/regime_detector.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace trade_ngin {

/**
 * @brief Detailed market regime classifications
 */
enum class DetailedMarketRegime {
    // Trend regimes
    STRONG_UPTREND,
    WEAK_UPTREND,
    STRONG_DOWNTREND,
    WEAK_DOWNTREND,
    
    // Mean reversion regimes
    HIGH_MEAN_REVERSION,
    LOW_MEAN_REVERSION,
    
    // Volatility regimes
    HIGH_VOLATILITY,
    LOW_VOLATILITY,
    VOLATILITY_EXPANSION,
    VOLATILITY_CONTRACTION,
    
    // Correlation regimes
    HIGH_CORRELATION,
    LOW_CORRELATION,
    CORRELATION_BREAKDOWN,
    
    // Liquidity regimes
    HIGH_LIQUIDITY,
    LOW_LIQUIDITY,
    LIQUIDITY_CRISIS,
    
    // Market stress regimes
    NORMAL,
    STRESS,
    CRISIS,
    
    UNDEFINED
};

/**
 * @brief Market regime features
 */
struct RegimeFeatures {
    double trend_strength{0.0};           // Measured by directional movement
    double mean_reversion_strength{0.0};  // Measured by variance ratio
    double volatility{0.0};               // Realized volatility
    double volatility_of_volatility{0.0}; // Vol of vol measure
    double correlation{0.0};              // Average pairwise correlation
    double liquidity{0.0};                // Market depth/resilience
    double market_stress{0.0};            // Composite stress indicator
    
    // Additional indicators
    double hurst_exponent{0.0};          // Long memory indicator
    double rsi{0.0};                     // Relative Strength Index
    double volume_profile{0.0};          // Volume distribution metric
    double bid_ask_spread{0.0};          // Average spread
    double order_flow_imbalance{0.0};    // Order flow metric
};

/**
 * @brief Configuration for regime detection
 */
struct RegimeDetectorConfig {
    int lookback_period{252};            // Historical lookback
    double confidence_threshold{0.75};    // Required confidence for regime change
    int min_regime_duration{5};          // Minimum days for regime
    bool use_machine_learning{false};     // Use ML models
    double var_ratio_threshold{2.0};      // Variance ratio threshold
    double correlation_threshold{0.7};    // High correlation threshold
    double volatility_threshold{0.3};     // High volatility threshold
    std::vector<std::string> features;    // Features to use
    std::string model_path;              // Path to ML models
};

/**
 * @brief Result of regime detection
 */
struct RegimeDetectionResult {
    DetailedMarketRegime current_regime;
    DetailedMarketRegime previous_regime;
    RegimeFeatures features;
    double regime_probability;
    int regime_duration;
    Timestamp last_change;
    std::string change_reason;
};

/**
 * @brief Detector for market regimes
 */
class RegimeDetector {
public:
    explicit RegimeDetector(RegimeDetectorConfig config);

    /**
     * @brief Initialize the detector
     * @return Result indicating success or failure
     */
    Result<void> initialize();

    /**
     * @brief Update detector with new market data
     * @param data Vector of price bars
     * @return Result indicating success or failure
     */
    Result<void> update(const std::vector<Bar>& data);

    /**
     * @brief Get current regime for a symbol
     * @param symbol Instrument symbol
     * @return Result containing regime detection result
     */
    Result<RegimeDetectionResult> get_regime(const std::string& symbol) const;

    /**
     * @brief Get regime features for a symbol
     * @param symbol Instrument symbol
     * @return Result containing regime features
     */
    Result<RegimeFeatures> get_features(const std::string& symbol) const;

    /**
     * @brief Check if regime has changed
     * @param symbol Instrument symbol
     * @return Result containing true if regime changed
     */
    Result<bool> has_regime_changed(const std::string& symbol) const;

    /**
     * @brief Get regime change probability
     * @param symbol Instrument symbol
     * @return Result containing probability of regime change
     */
    Result<double> get_change_probability(const std::string& symbol) const;

    /**
     * @brief Get regime history for a symbol
     * @param symbol Instrument symbol
     * @return Result containing vector of past regimes
     */
    Result<std::vector<RegimeDetectionResult>> get_regime_history(
        const std::string& symbol) const;

private:
    RegimeDetectorConfig config_;
    std::unordered_map<std::string, std::vector<double>> price_history_;
    std::unordered_map<std::string, std::vector<double>> volume_history_;
    std::unordered_map<std::string, RegimeDetectionResult> current_regimes_;
    std::unordered_map<std::string, std::vector<RegimeDetectionResult>> regime_history_;
    mutable std::mutex mutex_;

    /**
     * @brief Calculate trend features
     * @param prices Price history
     * @return Trend strength indicator
     */
    double calculate_trend_strength(const std::vector<double>& prices) const;

    /**
     * @brief Calculate mean reversion features
     * @param prices Price history
     * @return Mean reversion strength
     */
    double calculate_mean_reversion(const std::vector<double>& prices) const;

    /**
     * @brief Calculate volatility features
     * @param prices Price history
     * @return Realized volatility
     */
    double calculate_volatility(const std::vector<double>& prices) const;

    /**
     * @brief Calculate Hurst exponent
     * @param prices Price history
     * @return Hurst exponent value
     */
    double calculate_hurst(const std::vector<double>& prices) const;

    /**
     * @brief Calculate variance ratio
     * @param prices Price history
     * @param lags Vector of lags to use
     * @return Variance ratio statistic
     */
    double calculate_variance_ratio(
        const std::vector<double>& prices,
        const std::vector<int>& lags) const;

    /**
     * @brief Detect regime changes
     * @param features Current regime features
     * @param symbol Instrument symbol
     * @return New regime classification
     */
    DetailedMarketRegime detect_regime_change(
        const RegimeFeatures& features,
        const std::string& symbol) const;

    /**
     * @brief Calculate regime change probability
     * @param features Current regime features
     * @param current_regime Current regime
     * @return Probability of regime change
     */
    double calculate_change_probability(
        const RegimeFeatures& features,
        DetailedMarketRegime current_regime) const;

    /**
     * @brief Validate regime change
     * @param symbol Instrument symbol
     * @param new_regime Proposed new regime
     * @param features Current features
     * @return true if change is valid
     */
    bool validate_regime_change(
        const std::string& symbol,
        DetailedMarketRegime new_regime,
        const RegimeFeatures& features) const;
};

} // namespace trade_ngin