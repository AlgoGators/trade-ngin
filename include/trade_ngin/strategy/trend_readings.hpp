#pragma once

#include <string>
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace trade_ngin {

/// What a positions file prints of one sleeve for one symbol: the sleeve's forecast, the
/// volatility estimate it sized the position on (the file's sizing_vol column: annualised, a
/// fraction, as of the bar the forecast was computed on; not the account's realised volatility
/// statistic) and four moving averages of its price series. `known` is false, and every figure
/// 0, for a sleeve that is not a trend sleeve and for a symbol the sleeve holds no data on.
struct TrendReadings {
    bool known{false};
    double forecast{0.0};
    double sizing_vol{0.0};
    double ema_8{0.0};
    double ema_32{0.0};
    double ema_64{0.0};
    double ema_256{0.0};
};

/// The readings of `sleeve` for `symbol`. Every trend sleeve is the one trend class (the fast
/// sleeve is a configuration of it), so the live and the backtest positions files read both
/// through this one function.
inline TrendReadings trend_readings_of(const StrategyInterface* sleeve, const std::string& symbol) {
    TrendReadings readings;
    const auto* trend = dynamic_cast<const TrendFollowingStrategy*>(sleeve);
    if (trend == nullptr)
        return readings;
    const InstrumentData* data = trend->get_instrument_data(symbol);
    if (data == nullptr)
        return readings;
    readings.known = true;
    readings.forecast = data->current_forecast;
    readings.sizing_vol = data->current_volatility;
    const auto emas = trend->get_ema_values(symbol, {8, 32, 64, 256});
    const auto ema = [&](int window) {
        const auto it = emas.find(window);
        return it == emas.end() ? 0.0 : it->second;
    };
    readings.ema_8 = ema(8);
    readings.ema_32 = ema(32);
    readings.ema_64 = ema(64);
    readings.ema_256 = ema(256);
    return readings;
}

}  // namespace trade_ngin
