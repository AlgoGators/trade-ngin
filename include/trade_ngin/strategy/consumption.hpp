#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

enum class StrategyConsumptionProfile { Unsupported, Base, Standard, Fast, Slow, MeanReversion };

struct StrategySymbolLimitRead {
    bool present{false};
    std::optional<double> value;
};

struct StrategyPositionLimitConsumption {
    bool supported{false};
    std::unordered_map<std::string, StrategySymbolLimitRead> symbols;
};

struct StrategyTradingMultiplierRead {
    bool present{false};
    std::optional<double> value;
};

struct StrategyRiskConsumption {
    bool supported{false};
    std::unordered_map<std::string, StrategyTradingMultiplierRead> trading_multipliers;
    std::optional<double> capital_allocation;
    std::optional<Decimal> risk_max_leverage;
    std::optional<double> fallback_config_max_leverage;
    std::optional<Decimal> risk_max_drawdown;
};

struct StrategyHistoryConsumption {
    std::optional<size_t> max_history_size;
    std::optional<std::vector<std::pair<int, int>>> ema_windows;
};

struct StrategyVolatilityConsumption {
    std::optional<int> vol_lookback_short;
    std::optional<size_t> max_history_size;
};

struct StrategyForecastConsumption {
    std::optional<std::vector<std::pair<int, int>>> ema_windows;
    std::optional<int> vol_lookback_short;
    std::optional<std::vector<std::pair<int, double>>> fdm;
};

struct StrategyRegimeConsumption {
    std::optional<int> vol_lookback_long;
};

struct StrategySizingConsumption {
    std::optional<double> capital_allocation;
    std::optional<double> max_leverage;
    std::optional<double> idm;
    std::optional<double> risk_target;
    std::optional<double> fx_rate;
    std::optional<double> max_symbol_concentration;
    std::unordered_map<std::string, StrategySymbolLimitRead> symbol_limits;
};

struct StrategyBufferConsumption {
    std::optional<bool> use_position_buffering;
    std::optional<double> weight;
    std::optional<double> capital_allocation;
    std::optional<double> idm;
    std::optional<double> risk_target;
    std::optional<double> fx_rate;
    std::optional<double> carver_buffer_floor;
    std::optional<double> carver_buffer_position_factor;
    std::unordered_map<std::string, StrategySymbolLimitRead> symbol_limits;
};

// Equity reads are recorded only at their actual branch; absent means not reached.
struct MeanReversionSymbolConsumption {
    std::optional<int> lookback_period;
    std::optional<int> vol_lookback;
    std::optional<size_t> maximum_price_history;
    std::optional<size_t> maximum_volatility_history;
    std::optional<double> entry_threshold;
    std::optional<double> exit_threshold;
    std::optional<bool> use_stop_loss;
    std::optional<double> stop_loss_pct;
    std::optional<double> capital_allocation;
    std::optional<double> position_size;
    std::optional<double> risk_target;
    std::optional<bool> allow_fractional_shares;
    std::optional<double> fractional_min_price;
    std::optional<double> fractional_min_adv;
    StrategySymbolLimitRead position_limit;
    bool position_limit_reached{false};
    bool sizing_reached{false};
    bool fractional_adv_reached{false};
    std::optional<size_t> volume_sample_count;
    std::optional<double> average_daily_volume;
    std::optional<bool> fractional_eligible;
    std::optional<bool> short_allowed;
};
struct MeanReversionConsumption {
    bool capacity_exceeded{false};
    std::unordered_map<std::string,MeanReversionSymbolConsumption> symbols;
};

struct StrategyConsumptionTrace {
    StrategyConsumptionProfile profile{StrategyConsumptionProfile::Unsupported};
    StrategyHistoryConsumption history;
    StrategyVolatilityConsumption volatility;
    StrategyForecastConsumption forecast;
    StrategyRegimeConsumption regime;
    StrategySizingConsumption sizing;
    StrategyBufferConsumption buffering;
    StrategyRiskConsumption base_risk;
    StrategyPositionLimitConsumption position_limits;
    MeanReversionConsumption mean_reversion;
};

}  // namespace trade_ngin
