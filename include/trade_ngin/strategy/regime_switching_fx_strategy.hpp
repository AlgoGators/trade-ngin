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

struct RegimeSwitchingFXConfig : public StrategyConfig {
    // Volatility calculation parameters
    int volatility_window = 30;

    // Performance ranking parameters
    int performance_lookback = 5;

    // Regime detection parameters
    double low_dispersion_threshold = -0.5;
    double high_dispersion_threshold = 0.5;
    int zscore_lookback = 252;

    // Position sizing parameters
    int num_long_positions = 2;
    int num_short_positions = 2;
    bool use_volatility_scaling = true;

    // Risk management parameters
    double stop_loss_pct = 0.05;

    // Trading universe
    std::vector<std::string> symbols = {"6C", "6A", "6J", "6B", "6E", "6M", "6N"};

    // Serialization methods
    nlohmann::json to_json() const override;
    void from_json(const nlohmann::json& j) override;
};

struct RegimeSwitchingFXData {
    std::string symbol;
    double weight = 1.0;
    double contract_size = 1.0;

    // Price and return data
    std::vector<double> price_history;
    std::vector<double> returns;

    // Volatility metrics
    double realized_volatility = 0.0;

    // Performance metrics
    double recent_return = 0.0;

    // Signal and position data
    double current_signal = 0.0;
    double target_position = 0.0;
    double scaled_position = 0.0;
};

enum class RegimeSwitchingFXMarketRegime {
    MOMENTUM,
    MEAN_REVERSION,
    UNDEFINED
};

class RegimeSwitchingFXStrategy : public BaseStrategy {
public:

    RegimeSwitchingFXStrategy(std::string& id,
                             RegimeSwitchingFXConfig& config,
                             std::shared_ptr<PostgresDatabase> db);


    Result<void> initialize() override;


    Result<void> on_data(const std::vector<Bar>& data) override;


    Result<void> validate_config() const override;

    // Core Calculation Methods

    double calculate_realized_volatility(const std::vector<double>& returns) const;

    double calculate_volatility_dispersion(const std::vector<double>& volatilities) const;

    double calculate_zscore(double value, const std::vector<double>& history) const;

    RegimeSwitchingFXMarketRegime determine_regime(double dispersion_zscore) const;


    double calculate_recent_return(const std::vector<double>& prices, int lookback) const;


    std::vector<std::pair<std::string, double>> rank_by_performance() const;

    void generate_signals(RegimeSwitchingFXMarketRegime regime);

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
    std::vector<double> dispersion_history_;
    double dispersion_zscore_ = 0.0;

    // Helper Methods

    void update_price_history(const std::string& symbol, double price);

    void update_returns(const std::string& symbol);

    bool has_sufficient_data() const;
};

}