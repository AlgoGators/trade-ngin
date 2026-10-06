#pragma once

#include <gtest/gtest.h>
#include <limits>

#include "trade_ngin/strategy/consumption.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"

namespace trade_ngin::testing {

template <typename TrendConfig>
void set_nondefault_trend_inputs(TrendConfig& config) {
    config.weight = 0.37;
    config.risk_target = 0.18;
    config.fx_rate = 1.23;
    config.idm = 1.7;
    config.max_symbol_concentration = 0.22;
    config.use_position_buffering = true;
    config.carver_buffer_floor = 0.75;
    config.carver_buffer_position_factor = 0.03;
    config.ema_windows = {{2, 8}, {4, 16}};
    config.vol_lookback_short = 0;
    config.vol_lookback_long = 1;
    config.max_history_size = 0;
    config.fdm = {{2, 1.42}};
}

template <typename Strategy>
class SignalInspectableStrategy : public Strategy {
public:
    using Strategy::Strategy;

    const auto& recorded_signals() const { return this->last_signals_; }
};

template <typename Strategy>
void expect_signal_and_metrics_parity(const Strategy& observed, const Strategy& plain,
                                      const std::string& symbol) {
    const auto& observed_signals = observed.recorded_signals();
    const auto& plain_signals = plain.recorded_signals();
    ASSERT_EQ(observed_signals.size(), size_t{1});
    ASSERT_EQ(plain_signals.size(), size_t{1});
    ASSERT_EQ(observed_signals.count(symbol), size_t{1});
    ASSERT_EQ(plain_signals.count(symbol), size_t{1});
    EXPECT_DOUBLE_EQ(observed_signals.at(symbol), plain_signals.at(symbol));

    const auto& a = observed.get_metrics();
    const auto& b = plain.get_metrics();
    EXPECT_DOUBLE_EQ(a.unrealized_pnl, b.unrealized_pnl);
    EXPECT_DOUBLE_EQ(a.realized_pnl, b.realized_pnl);
    EXPECT_DOUBLE_EQ(a.total_pnl, b.total_pnl);
    EXPECT_DOUBLE_EQ(a.sharpe_ratio, b.sharpe_ratio);
    EXPECT_DOUBLE_EQ(a.sortino_ratio, b.sortino_ratio);
    EXPECT_DOUBLE_EQ(a.max_drawdown, b.max_drawdown);
    EXPECT_DOUBLE_EQ(a.win_rate, b.win_rate);
    EXPECT_DOUBLE_EQ(a.profit_factor, b.profit_factor);
    EXPECT_EQ(a.total_trades, b.total_trades);
    EXPECT_DOUBLE_EQ(a.avg_trade, b.avg_trade);
    EXPECT_DOUBLE_EQ(a.avg_winner, b.avg_winner);
    EXPECT_DOUBLE_EQ(a.avg_loser, b.avg_loser);
    EXPECT_DOUBLE_EQ(a.max_winner, b.max_winner);
    EXPECT_DOUBLE_EQ(a.max_loser, b.max_loser);
    EXPECT_DOUBLE_EQ(a.avg_holding_period, b.avg_holding_period);
    EXPECT_DOUBLE_EQ(a.turnover, b.turnover);
    EXPECT_DOUBLE_EQ(a.volatility, b.volatility);
}

inline void expect_malformed_bar_error_parity(StrategyInterface& observed,
                                              StrategyInterface& plain,
                                              std::vector<Bar> bars,
                                              StrategyConsumptionTrace& trace) {
    ASSERT_FALSE(bars.empty());
    bars.front().open = Decimal(0.0);
    auto observed_result = observed.on_data(bars, &trace);
    auto plain_result = plain.on_data(bars);
    ASSERT_TRUE(observed_result.is_error());
    ASSERT_TRUE(plain_result.is_error());
    EXPECT_EQ(observed_result.error()->code(), ErrorCode::INVALID_DATA);
    EXPECT_EQ(observed_result.error()->code(), plain_result.error()->code());
    const std::string observed_message = observed_result.error()->what();
    const std::string plain_message = plain_result.error()->what();
    EXPECT_FALSE(observed_message.empty());
    EXPECT_EQ(observed_message, plain_message);
    EXPECT_FALSE(trace.history.max_history_size.has_value());
}

inline void expect_full_strategy_consumption(const StrategyConsumptionTrace& trace,
                                             StrategyConsumptionProfile profile,
                                             int normalized_short, int normalized_long) {
    EXPECT_EQ(trace.profile, profile);
    ASSERT_TRUE(trace.history.max_history_size.has_value());
    EXPECT_EQ(*trace.history.max_history_size, size_t{756});
    const std::vector<std::pair<int, int>> windows{{2, 8}, {4, 16}};
    ASSERT_TRUE(trace.history.ema_windows.has_value());
    EXPECT_EQ(*trace.history.ema_windows, windows);
    ASSERT_TRUE(trace.volatility.vol_lookback_short.has_value());
    EXPECT_EQ(*trace.volatility.vol_lookback_short, normalized_short);
    ASSERT_TRUE(trace.volatility.max_history_size.has_value());
    EXPECT_EQ(*trace.volatility.max_history_size, size_t{756});
    ASSERT_TRUE(trace.forecast.ema_windows.has_value());
    EXPECT_EQ(*trace.forecast.ema_windows, windows);
    ASSERT_TRUE(trace.forecast.vol_lookback_short.has_value());
    EXPECT_EQ(*trace.forecast.vol_lookback_short, normalized_short);
    ASSERT_TRUE(trace.forecast.fdm.has_value());
    EXPECT_EQ(*trace.forecast.fdm, (std::vector<std::pair<int, double>>{{2, 1.42}}));
    ASSERT_TRUE(trace.regime.vol_lookback_long.has_value());
    EXPECT_EQ(*trace.regime.vol_lookback_long, normalized_long);

    ASSERT_TRUE(trace.sizing.capital_allocation.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.capital_allocation, 1'000'000.0);
    ASSERT_TRUE(trace.sizing.max_leverage.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.max_leverage, 100.0);
    ASSERT_TRUE(trace.sizing.idm.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.idm, 1.7);
    ASSERT_TRUE(trace.sizing.risk_target.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.risk_target, 0.18);
    ASSERT_TRUE(trace.sizing.fx_rate.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.fx_rate, 1.23);
    ASSERT_TRUE(trace.sizing.max_symbol_concentration.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.max_symbol_concentration, 0.22);
    ASSERT_EQ(trace.sizing.symbol_limits.count("ES"), size_t{1});
    ASSERT_TRUE(trace.sizing.symbol_limits.at("ES").value.has_value());
    EXPECT_DOUBLE_EQ(*trace.sizing.symbol_limits.at("ES").value, 1000.0);

    ASSERT_TRUE(trace.buffering.use_position_buffering.has_value());
    EXPECT_TRUE(*trace.buffering.use_position_buffering);
    ASSERT_TRUE(trace.buffering.weight.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.weight, 0.37);
    ASSERT_TRUE(trace.buffering.capital_allocation.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.capital_allocation, 1'000'000.0);
    ASSERT_TRUE(trace.buffering.idm.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.idm, 1.7);
    ASSERT_TRUE(trace.buffering.risk_target.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.risk_target, 0.18);
    ASSERT_TRUE(trace.buffering.fx_rate.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.fx_rate, 1.23);
    ASSERT_TRUE(trace.buffering.carver_buffer_floor.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.carver_buffer_floor, 0.75);
    ASSERT_TRUE(trace.buffering.carver_buffer_position_factor.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.carver_buffer_position_factor, 0.03);
    ASSERT_EQ(trace.buffering.symbol_limits.count("ES"), size_t{1});
    ASSERT_TRUE(trace.buffering.symbol_limits.at("ES").value.has_value());
    EXPECT_DOUBLE_EQ(*trace.buffering.symbol_limits.at("ES").value, 1000.0);

    EXPECT_TRUE(trace.base_risk.supported);
    ASSERT_TRUE(trace.base_risk.risk_max_leverage.has_value());
    EXPECT_DOUBLE_EQ(static_cast<double>(*trace.base_risk.risk_max_leverage), 3.25);
    ASSERT_TRUE(trace.base_risk.risk_max_drawdown.has_value());
    EXPECT_DOUBLE_EQ(static_cast<double>(*trace.base_risk.risk_max_drawdown), 0.45);
    EXPECT_FALSE(trace.base_risk.fallback_config_max_leverage.has_value());
    ASSERT_TRUE(trace.base_risk.capital_allocation.has_value());
    EXPECT_DOUBLE_EQ(*trace.base_risk.capital_allocation, 1'000'000.0);
    ASSERT_EQ(trace.position_limits.symbols.count("ES"), size_t{1});
    ASSERT_TRUE(trace.position_limits.symbols.at("ES").value.has_value());
    EXPECT_DOUBLE_EQ(*trace.position_limits.symbols.at("ES").value, 1000.0);
}

template <typename Strategy>
void expect_safe_helper_early_returns(Strategy& strategy) {
    StrategyConsumptionTrace trace;
    EXPECT_DOUBLE_EQ(strategy.calculate_position("ES", std::numeric_limits<double>::quiet_NaN(),
                                                100.0, 0.1, &trace), 0.0);
    EXPECT_FALSE(trace.sizing.capital_allocation.has_value());
    trace = {};
    EXPECT_DOUBLE_EQ(strategy.apply_position_buffer(
                         "ES", std::numeric_limits<double>::quiet_NaN(), 100.0, 0.1, &trace),
                     0.0);
    ASSERT_TRUE(trace.buffering.use_position_buffering.has_value());
    EXPECT_TRUE(*trace.buffering.use_position_buffering);
    EXPECT_FALSE(trace.buffering.weight.has_value());
    EXPECT_FALSE(trace.buffering.carver_buffer_floor.has_value());
    trace = {};
    EXPECT_TRUE(strategy.get_scaled_combined_forecast({}, &trace).empty());
    EXPECT_FALSE(trace.forecast.fdm.has_value());
    trace = {};
    EXPECT_DOUBLE_EQ(strategy.calculate_vol_regime_multiplier({100.0}, {0.1}, &trace), 2.0 / 3.0);
    EXPECT_FALSE(trace.regime.vol_lookback_long.has_value());
}

inline void expect_missing_symbol_limit(const StrategyConsumptionTrace& trace) {
    ASSERT_EQ(trace.sizing.symbol_limits.count("ES"), size_t{1});
    EXPECT_FALSE(trace.sizing.symbol_limits.at("ES").present);
    EXPECT_FALSE(trace.sizing.symbol_limits.at("ES").value.has_value());
    ASSERT_EQ(trace.buffering.symbol_limits.count("ES"), size_t{1});
    EXPECT_FALSE(trace.buffering.symbol_limits.at("ES").present);
    EXPECT_FALSE(trace.buffering.symbol_limits.at("ES").value.has_value());
    EXPECT_TRUE(trace.position_limits.supported);
    ASSERT_EQ(trace.position_limits.symbols.count("ES"), size_t{1});
    EXPECT_FALSE(trace.position_limits.symbols.at("ES").present);
    EXPECT_FALSE(trace.position_limits.symbols.at("ES").value.has_value());
}

inline void expect_full_to_short_empty_error_reset(StrategyInterface& selected,
                                                    std::vector<Bar> short_bars,
                                                    StrategyConsumptionProfile profile) {
    ASSERT_FALSE(short_bars.empty());
    StrategyConsumptionTrace trace;
    ASSERT_TRUE(selected.on_data(short_bars, &trace).is_ok());
    EXPECT_EQ(trace.profile, profile);
    EXPECT_TRUE(trace.history.max_history_size.has_value());
    EXPECT_TRUE(trace.history.ema_windows.has_value());
    EXPECT_TRUE(trace.base_risk.supported);
    EXPECT_FALSE(trace.volatility.vol_lookback_short.has_value());
    EXPECT_FALSE(trace.forecast.ema_windows.has_value());
    EXPECT_FALSE(trace.sizing.capital_allocation.has_value());
    EXPECT_FALSE(trace.buffering.use_position_buffering.has_value());

    ASSERT_TRUE(selected.on_data({}, &trace).is_ok());
    EXPECT_EQ(trace.profile, profile);
    EXPECT_FALSE(trace.history.max_history_size.has_value());
    EXPECT_FALSE(trace.base_risk.supported);

    short_bars.front().open = Decimal(0.0);
    auto invalid = selected.on_data(short_bars, &trace);
    ASSERT_TRUE(invalid.is_error());
    EXPECT_EQ(invalid.error()->code(), ErrorCode::INVALID_DATA);
    EXPECT_EQ(trace.profile, profile);
    EXPECT_FALSE(trace.history.max_history_size.has_value());
    EXPECT_FALSE(trace.base_risk.supported);
    EXPECT_FALSE(trace.forecast.ema_windows.has_value());
}

}  // namespace trade_ngin::testing
