// src/strategy/forecast_scaler.cpp

#include "trade_ngin/strategy/forecast_scaler.hpp"
#include "trade_ngin/core/logger.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace trade_ngin {

ForecastScaler::ForecastScaler(ForecastScalerConfig config)
    : config_(std::move(config)) {}

void ForecastScaler::update_volatility(
    const std::string& symbol,
    double volatility) {
    
    // Add new volatility to history
    auto& history = volatility_history_[symbol];
    history.push_back(volatility);

    // Maintain lookback window
    if (history.size() > static_cast<size_t>(config_.volatility_lookback)) {
        history.pop_front();
    }

    // Calculate new quantile and multiplier
    double quantile = calculate_quantile(symbol);
    double new_multiplier = calculate_multiplier(quantile);

    // Update EWMA multiplier
    update_ewma_multiplier(symbol, new_multiplier);
}

Result<double> ForecastScaler::scale_forecast(
    const std::string& symbol,
    double raw_forecast,
    ForecastType type) const {
    
    try {
        // Get current volatility multiplier
        auto mult_result = get_multiplier(symbol);
        if (mult_result.is_error()) {
            return make_error<double>(
                mult_result.error()->code(),
                mult_result.error()->what(),
                "ForecastScaler"
            );
        }

        double multiplier = mult_result.value();

        // Apply volatility-based adjustment
        double adjusted_forecast = raw_forecast * multiplier;

        // Apply base scalar based on forecast type
        double base_scalar = (type == ForecastType::TREND) ?
            config_.base_scalar_trend : config_.base_scalar_carry;
        
        double scaled_forecast = adjusted_forecast * base_scalar;

        // Apply forecast cap
        return Result<double>(
            std::max(-config_.forecast_cap,
                    std::min(config_.forecast_cap, scaled_forecast))
        );

    } catch (const std::exception& e) {
        return make_error<double>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error scaling forecast: ") + e.what(),
            "ForecastScaler"
        );
    }
}

Result<double> ForecastScaler::get_multiplier(
    const std::string& symbol) const {
    
    auto it = multiplier_history_.find(symbol);
    if (it == multiplier_history_.end() || it->second.empty()) {
        return make_error<double>(
            ErrorCode::INVALID_ARGUMENT,
            "No multiplier history for symbol: " + symbol,
            "ForecastScaler"
        );
    }

    return Result<double>(it->second.back());
}

Result<double> ForecastScaler::get_quantile(
    const std::string& symbol) const {
    
    if (volatility_history_.find(symbol) == volatility_history_.end()) {
        return make_error<double>(
            ErrorCode::INVALID_ARGUMENT,
            "No volatility history for symbol: " + symbol,
            "ForecastScaler"
        );
    }

    return Result<double>(calculate_quantile(symbol));
}

double ForecastScaler::calculate_quantile(
    const std::string& symbol) const {
    
    // Use .find() to check for existence first
    auto it = volatility_history_.find(symbol);
    if (it == volatility_history_.end() || it->second.empty()) {
        return 2.0 / 3.0;  // Default to median if no history
    }

    // Use the iterator to access the history
    const auto& history = it->second;
    // Get current volatility
    double current_vol = history.back();

    // Count how many historical values are below current
    size_t count_below = std::count_if(
        history.begin(), history.end(),
        [current_vol](double vol) { return vol < current_vol; }
    );

    // Calculate quantile
    return static_cast<double>(count_below) / (history.size() - 1);
}

double ForecastScaler::calculate_multiplier(double quantile) const {
    // Implement the formula: M = 2 - 1.5 * Q
    return 2.0 - 1.5 * quantile;
}

void ForecastScaler::update_ewma_multiplier(
    const std::string& symbol,
    double new_multiplier) {
    
    auto& history = multiplier_history_[symbol];
    
    if (history.empty()) {
        history.push_back(new_multiplier);
        return;
    }

    // Calculate EWMA
    double alpha = 2.0 / (config_.ewma_decay + 1.0);
    double ewma = alpha * new_multiplier + (1.0 - alpha) * history.back();
    
    history.push_back(ewma);

    // Keep same size as volatility history
    if (history.size() > static_cast<size_t>(config_.volatility_lookback)) {
        history.pop_front();
    }
}

} // namespace trade_ngin