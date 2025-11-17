#pragma once

#include <memory>
#include <utility>
#include <vector>
#include <string>
#include <unordered_map>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

// Strategy Configuration
struct RegimeSwitchingFXConfig : public StrategyConfig {
    // Volatility calculation parameters
    int volatility_window = 30;  // 30-day rolling volatility

    // Performance ranking parameters (for momentum regime)
    int momentum_lookback = 120;  // 120-day returns for momentum ranking

    // EWMAC parameters (for mean reversion regime)
    int ewmac_short_lookback = 8;   // Short-term EWMAC window
    int ewmac_long_lookback = 32;   // Long-term EWMAC window

    // Regime detection parameters
    int zscore_lookback = 60;  // 60-day z-score window
    double regime_threshold = 0.5;  // Threshold for regime classification

    // Position sizing parameters
    int num_long_positions = 2;
    int num_short_positions = 2;
    bool use_volatility_scaling = true;

    // Rebalancing parameters
    int momentum_rebalance_days = 20;  // Rebalance every 20 days in momentum
    int mean_reversion_rebalance_days = 5;  // Rebalance every 5 days in mean reversion

    // Risk management parameters
    double stop_loss_pct = 0.10;

    // Trading universe
    std::vector<std::string> symbols = {"6C", "6A", "6J", "6B", "6E", "6M", "6N"};

    // Serialization methods
    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

// Per-instrument data structure
struct RegimeSwitchingFXData {
    std::string symbol;
    double weight = 1.0;
    double contract_size = 1.0;

    // Price and return data
    std::vector<double> price_history;
    std::vector<double> log_returns;

    // Volatility metrics
    std::vector<double> rolling_volatilities;
    double current_volatility = 0.0;

    // EWMAC indicators
    std::vector<double> ewmac_values;
    double current_ewmac = 0.0;

    // Performance metrics
    double recent_return = 0.0;  // N-day return for momentum ranking

    // Signal and position data
    double current_signal = 0.0;
    double target_position = 0.0;
    double scaled_position = 0.0;

    Timestamp last_update;
};

// Market regime enumeration
enum class RegimeSwitchingFXMarketRegime {
    MOMENTUM,
    MEAN_REVERSION,
    UNDEFINED
};

// Main strategy class
class RegimeSwitchingFXStrategy : public BaseStrategy {
public:
    // Constructor - required signature for trade-ngin framework
    RegimeSwitchingFXStrategy(std::string id,
                             RegimeSwitchingFXConfig config,
                             std::shared_ptr<PostgresDatabase> db);

    // Required BaseStrategy overrides
    Result<void> initialize() override;
    Result<void> on_data(const std::vector<Bar>& data) override;
    Result<void> validate_config() const override;

    // Core Calculation Methods

    // Calculate log returns from price series
    std::vector<double> calculate_log_returns(const std::vector<double>& prices) const;

    // Calculate rolling volatility (30-day standard deviation of returns)
    std::vector<double> calculate_rolling_volatility(
        const std::vector<double>& returns, int window) const;

    // Calculate cross-sectional volatility dispersion
    double calculate_volatility_dispersion(
        const std::vector<double>& volatilities) const;

    // Calculate z-score for regime detection
    double calculate_zscore(double value, const std::vector<double>& history) const;

    // Determine market regime from dispersion z-score
    RegimeSwitchingFXMarketRegime determine_regime(double dispersion_zscore) const;

    // Calculate N-day log return for performance ranking
    double calculate_n_day_return(
        const std::vector<double>& prices, size_t current_idx, int lookback) const;

    // Calculate EWMAC (Exponentially Weighted Moving Average Crossover)
    std::vector<double> calculate_ewmac(
        const std::vector<double>& prices, int short_window, int long_window) const;

    // Rank instruments by performance metric
    std::vector<std::pair<std::string, double>> rank_by_performance() const;

    // Rank instruments by EWMAC values
    std::vector<std::pair<std::string, double>> rank_by_ewmac() const;

    // Generate trading signals based on regime
    void generate_signals(RegimeSwitchingFXMarketRegime regime);

    // Position sizing and risk management
    double calculate_position_size(const std::string& symbol, double signal) const;
    double apply_volatility_scaling(const std::string& symbol, double position) const;
    double apply_risk_controls(const std::string& symbol, double position) const;

    // Public Accessor Methods
    RegimeSwitchingFXMarketRegime get_current_regime() const { return current_regime_; }
    double get_dispersion_zscore() const { return dispersion_zscore_; }
    double get_signal(const std::string& symbol) const;
    double get_position(const std::string& symbol) const;
    const RegimeSwitchingFXData* get_instrument_data(const std::string& symbol) const;

private:
    // Strategy configuration
    RegimeSwitchingFXConfig fx_config_;

    // Per-instrument data storage
    std::unordered_map<std::string, RegimeSwitchingFXData> instrument_data_;

    // Regime state
    RegimeSwitchingFXMarketRegime current_regime_ = RegimeSwitchingFXMarketRegime::UNDEFINED;
    RegimeSwitchingFXMarketRegime previous_regime_ = RegimeSwitchingFXMarketRegime::UNDEFINED;
    std::vector<double> dispersion_history_;
    double dispersion_zscore_ = 0.0;

    // Rebalancing state
    int days_since_last_rebalance_ = 0;

    // Helper Methods
    void update_price_history(const std::string& symbol, double price);
    void update_returns(const std::string& symbol);
    void update_volatilities();
    void update_ewmac_values();
    void update_regime();
    bool has_sufficient_data() const;
    bool should_rebalance() const;

    // Statistical utility functions
    double calculate_mean(const std::vector<double>& values) const;
    double calculate_variance(const std::vector<double>& values) const;
    double calculate_stdev(const std::vector<double>& values) const;
};

}