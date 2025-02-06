// src/strategy/trend_following.cpp
#include "trade_ngin/strategy/trend_following.hpp"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <iostream>

namespace trade_ngin {

TrendFollowingStrategy::TrendFollowingStrategy(
    std::string id,
    StrategyConfig config,
    TrendFollowingConfig trend_config,
    std::shared_ptr<DatabaseInterface> db)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
    trend_config_(std::move(trend_config)) {  // Move trend_config after using its config
    
    // Initialize metadata
    metadata_.name = "Trend Following Strategy";
    metadata_.description = "Multi-timeframe trend following using EMA crossovers";
}

Result<void> TrendFollowingStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error()) return result;
    
    // Validate trend-specific config
    if (trend_config_.risk_target <= 0.0 || trend_config_.risk_target > 1.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Risk target must be between 0 and 1",
            "TrendFollowingStrategy"
        );
    }
    
    if (trend_config_.idm <= 0.0) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "IDM must be positive",
            "TrendFollowingStrategy"
        );
    }
    
    if (trend_config_.ema_windows.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Must specify at least one EMA window pair",
            "TrendFollowingStrategy"
        );
    }
    
    return Result<void>();
}

Result<void> TrendFollowingStrategy::initialize() {
    // Call base class initialization first
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        return base_result;
    }

    try {
        // Initialize price history containers for each symbol
        for (const auto& config_pair : config_.trading_params) {
            const auto& symbol = config_pair.first;
            price_history_[symbol] = std::vector<double>();
            volatility_history_[symbol] = std::vector<double>();
        }

        // Validate trend-specific configuration
        auto validate_result = validate_config();
        if (validate_result.is_error()) {
            return validate_result;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Failed to initialize trend following strategy: ") + e.what(),
            "TrendFollowingStrategy"
        );
    }
}

Result<void> TrendFollowingStrategy::on_data(const std::vector<Bar>& data) {
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error()) return base_result;
    
    try {
        // Group data by symbol and update price history
        for (const auto& bar : data) {
            price_history_[bar.symbol].push_back(bar.close);
            
            // Only process if we have enough data
            if (price_history_[bar.symbol].size() > 256) { // Minimum required for longest EMA
                const auto& prices = price_history_[bar.symbol];
                
                // Calculate volatility estimate
                auto volatility = calculate_volatility(
                    prices,
                    trend_config_.vol_lookback_short,
                    trend_config_.vol_lookback_long
                );
                volatility_history_[bar.symbol] = volatility;
                
                // Generate forecast
                auto forecast = generate_raw_forecasts(prices, volatility);
                
                // Calculate position
                double raw_position = calculate_position(
                    bar.symbol,
                    forecast.back(), // Use latest forecast
                    bar.close,
                    volatility.back()
                );
                
                // Apply buffering if enabled
                double final_position = trend_config_.use_position_buffering
                    ? apply_position_buffer(bar.symbol, raw_position, bar.close, volatility.back())
                    : raw_position;
                
                // Save forecast
                auto signal_result = on_signal(bar.symbol, forecast.back());
                if (signal_result.is_error()) {
                    return signal_result;
                }
                
                // Update position
                Position pos;
                pos.symbol = bar.symbol;
                pos.quantity = final_position;
                pos.average_price = bar.close;
                pos.last_update = bar.timestamp;
                
                auto pos_result = update_position(bar.symbol, pos);
                if (pos_result.is_error()) {
                    return pos_result;
                } 
            }
        }
        
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Error processing data: ") + e.what(),
            "TrendFollowingStrategy"
        );
    }
}

std::vector<double> TrendFollowingStrategy::get_single_scaled_forecast(
    const std::vector<double>& prices,
    int short_window,
    int long_window) const {
    
    // Calculate EMAs
    std::vector<double> short_ema(prices.size());
    std::vector<double> long_ema(prices.size());
    
    // EMA multipliers
    double short_alpha = 2.0 / (short_window + 1);
    double long_alpha = 2.0 / (long_window + 1);
    
    // Initialize
    short_ema[0] = prices[0];
    long_ema[0] = prices[0];
    
    // Calculate EMAs
    for (size_t i = 1; i < prices.size(); ++i) {
        short_ema[i] = prices[i] * short_alpha + short_ema[i-1] * (1 - short_alpha);
        long_ema[i] = prices[i] * long_alpha + long_ema[i-1] * (1 - long_alpha);
    }

    // Get volatility series
    auto volatility = calculate_volatility(
        prices,
        trend_config_.vol_lookback_short,
        trend_config_.vol_lookback_long
    );

    // If any volatility is 0, fail system using custom error, else use volatility
    if (std::any_of(volatility.begin(), volatility.end(), [](double v) { return v == 0.0; })) {
        throw std::runtime_error("Zero volatility detected");
    }

    // Calculate crossover signals and apply volatility multiplier
    std::vector<double> forecasts(prices.size());
    double vol_multiplier = calculate_vol_regime_multiplier(prices, volatility);

    for (size_t i = 0; i < prices.size(); ++i) {
        forecasts[i] = (short_ema[i] - long_ema[i]) / (prices[i] * volatility[i] / 16.0);
        forecasts[i] *= vol_multiplier;
    }

    // Get average absolute value of forecasts
    double abs_sum = std::accumulate(forecasts.begin(), forecasts.end(), 0.0,
        [](double acc, double val) { return acc + std::abs(val); });
    double abs_avg = abs_sum / forecasts.size();

    // Scale forecasts by (10 / average absolute value)
    for (size_t i = 0; i < forecasts.size(); ++i) {
        forecasts[i] *= 10.0 / abs_avg;
    }

    // Cap forecasts between -20 and 20
    for (size_t i = 0; i < forecasts.size(); ++i) {
        forecasts[i] = std::max(-20.0, std::min(20.0, forecasts[i]));
    }
    
    return forecasts;
}

std::vector<double> TrendFollowingStrategy::calculate_volatility(
    const std::vector<double>& prices,
    int short_lookback,
    int long_lookback) const {
    
    // Need at least 1 year of data
    if (prices.size() < 252) {
        return std::vector<double>(prices.size(), std::numeric_limits<double>::quiet_NaN());
    }

    std::vector<double> returns(prices.size() - 1);
    for (size_t i = 1; i < prices.size(); ++i) {
        returns[i-1] = std::log(prices[i] / prices[i-1]);
    }
    
    std::vector<double> volatility(prices.size(), 0.0);
    
    // Calculate rolling standard deviations
    for (size_t i = short_lookback; i < prices.size(); ++i) {
        // Short-term volatility
        // First calculate mean of returns in the window
        double short_mean = 0.0;
        for (int j = 0; j < short_lookback; ++j) {
            short_mean += returns[i-j-1];
        }
        short_mean /= short_lookback;
        
        // Then calculate variance
        double short_var = 0.0;
        for (int j = 0; j < short_lookback; ++j) {
            double deviation = returns[i-j-1] - short_mean;
            short_var += deviation * deviation;
        }
        short_var /= (short_lookback - 1);
        
        // Calculate adaptive long-term lookback
        size_t available_history = i + 1;
        int adaptive_long_lookback = std::min(
            static_cast<size_t>(long_lookback),
            std::max(static_cast<size_t>(252),
                    available_history)
        );
        
        // Long-term volatility
        // First calculate mean
        double long_mean = 0.0;
        for (int j = 0; j < adaptive_long_lookback; ++j) {
            if (i-j-1 < returns.size()) {
                long_mean += returns[i-j-1];
            }
        }
        long_mean /= adaptive_long_lookback;
        
        // Then calculate variance
        double long_var = 0.0;
        for (int j = 0; j < adaptive_long_lookback; ++j) {
            if (i-j-1 < returns.size()) {
                double deviation = returns[i-j-1] - long_mean;
                long_var += deviation * deviation;
            }
        }
        long_var /= (adaptive_long_lookback - 1);
        
        // Blend volatilities and annualize
        volatility[i] = std::sqrt(0.7 * short_var + 0.3 * long_var) * std::sqrt(252.0);
    }
    
    return volatility;
}

std::vector<double> TrendFollowingStrategy::generate_raw_forecasts(
    const std::vector<double>& prices,
    const std::vector<double>& volatility) const {
    
    std::vector<std::vector<double>> all_forecasts;
    
    // Calculate forecasts for each EMA pair
    for (const auto& window_pair : trend_config_.ema_windows) {
        auto single_rule_forecasts = get_single_scaled_forecast(
            prices,
            window_pair.first,
            window_pair.second
        );
        all_forecasts.push_back(single_rule_forecasts);
    }

    // Get FDM from trend config which depends on the number of EMA windows
    double fdm = trend_config_.fdm[all_forecasts.size()].second;
    
    // Iterate through each day
    std::vector<double> combined_forecasts(prices.size(), 0.0);
    for (size_t i = 0; i < prices.size(); ++i) {
        double sum = 0.0;
        bool all_valid = true;
        
        // Check forecasts from each EMA pair for this day
        for (const auto& forecasts : all_forecasts) {
            // If any forecast series doesn't have a valid forecast for this day,
            // mark as invalid and skip
            if (i < forecasts.size() && !std::isnan(forecasts[i])) {
                all_valid = false;
                break;
            }
            sum += forecasts[i];
        }
                
        // Only add combined forecast if all pairs contributed
        if (all_valid) {
            double raw_combined_forecast = (sum / all_forecasts.size());
            double scaled_combined_forecast = raw_combined_forecast * fdm;
            double capped_combined_forecast = std::max(-20.0, std::min(20.0, scaled_combined_forecast));

            combined_forecasts.push_back(capped_combined_forecast);
        } else {
            // Else, mark as NaN
            combined_forecasts.push_back(std::numeric_limits<double>::quiet_NaN());
        }
}

    return combined_forecasts;
}

double TrendFollowingStrategy::calculate_position(
    const std::string& symbol,
    double forecast,
    double price,
    double volatility) const {
    
    // Get contract specification
    const auto& contracts = config_.trading_params;
    double contract_size = contracts.count(symbol) ? contracts.at(symbol) : 1.0;
    
    // Calculate position using volatility targeting formula
    double position = (forecast * config_.capital_allocation * trend_config_.idm * trend_config_.risk_target) /
                     (10.0 * contract_size * price * trend_config_.fx_rate * volatility);
    
    return position;
}

double TrendFollowingStrategy::apply_position_buffer(
    const std::string& symbol,
    double raw_position,
    double price,
    double volatility) const {
    
    // Get contract specification
    const auto& contracts = config_.trading_params;
    double contract_size = contracts.count(symbol) ? contracts.at(symbol) : 1.0;

    // Calculate buffer width (10% of standard position size)
    double buffer_width = (0.1 * config_.capital_allocation * trend_config_.idm * trend_config_.risk_target) /
                         (contract_size * price * trend_config_.fx_rate * volatility);

    // Get current position
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = pos_it->second.quantity;
    }

    // Calculate buffer bounds
    double lower_buffer = raw_position - buffer_width;
    double upper_buffer = raw_position + buffer_width;

    // Apply buffering logic
    if (current_position < lower_buffer) {
        return std::round(lower_buffer);
    } else if (current_position > upper_buffer) {
        return std::round(upper_buffer);
    } else {
        return std::round(current_position);
    }
}

double TrendFollowingStrategy::calculate_vol_regime_multiplier(
    const std::vector<double>& prices,
    const std::vector<double>& volatility) const {
        if (prices.size() < 252) {  // Need at least 1 year of data
            return (2.0 / 3.0);  // Default multiplier if insufficient data
        }

        // Get current blended volatility (last value in volatility vector)
        double current_vol = volatility.back();

        // Calculate the lookback period for long-run average
        // Minimum 1 year (252), maximum 10 years (2520), scale with available data
        size_t max_lookback = 2520;  // 10 years
        size_t available_days = prices.size();
        size_t lookback = std::min(available_days, max_lookback);
        lookback = std::max(lookback, static_cast<size_t>(252));  // Minimum 1 year

        // Calculate long-run average volatility
        double sum_vol = 0.0;
        size_t count = 0;
        for (size_t i = volatility.size() - std::min(volatility.size(), lookback); 
            i < volatility.size(); ++i) {
            sum_vol += volatility[i];
            count++;
        }
        double avg_vol = sum_vol / count;

        // Calculate relative volatility level
        double relative_vol_level = current_vol / avg_vol;

        // Calculate quantile of relative volatility levels using historical values
        std::vector<double> historical_rel_vol_levels;
        historical_rel_vol_levels.reserve(lookback);
        
        for (size_t i = volatility.size() - std::min(volatility.size(), lookback);
            i < volatility.size() - 1; ++i) {  // Exclude current day
            historical_rel_vol_levels.push_back(volatility[i] / avg_vol);
        }

        // Sort to calculate quantile
        std::sort(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end());
        
        // Find position of current relative volatility level in sorted historical values
        auto it = std::upper_bound(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end(), relative_vol_level);
        double quantile = static_cast<double>(std::distance(historical_rel_vol_levels.begin(), it)) / 
                static_cast<double>(historical_rel_vol_levels.size());

        // TO:DO - ENSURE QUANTILE IS BETWEEN 0 AND 1 IN TESTING

        // Calculate raw multiplier using formula: 2 - 1.5 * Q
        double raw_multiplier = 2.0 - 1.5 * quantile;

        // Apply 10-day EWMA to the multiplier
        static const size_t EWMA_DAYS = 10;
        static const double alpha = 2.0 / (EWMA_DAYS + 1.0);
        
        // Default to 2/3 if insufficient data for EWMA
        if (historical_rel_vol_levels.size() < EWMA_DAYS) {
            return (2.0 / 3.0);
        }

        // Calculate EWMA of multiplier
        double ewma_vol_multiplier = raw_multiplier;
        double prev_ewma_vol_multiplier = ewma_vol_multiplier;
        
        for (size_t i = 0; i < EWMA_DAYS - 1; ++i) {
            size_t idx = volatility.size() - EWMA_DAYS + i;
            double historical_vol = volatility[idx];
            double historical_relative_vol = historical_vol / avg_vol;
            
            // Find quantile for this historical point
            auto hist_it = std::upper_bound(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end(), historical_relative_vol);
            double hist_Q = static_cast<double>(std::distance(historical_rel_vol_levels.begin(), hist_it)) / 
                        static_cast<double>(historical_rel_vol_levels.size());
            
            double hist_multiplier = 2.0 - 1.5 * hist_Q;
            
            // Update EWMA
            ewma_vol_multiplier = alpha * hist_multiplier + (1.0 - alpha) * prev_ewma_vol_multiplier;
            prev_ewma_vol_multiplier = ewma_vol_multiplier;
        }

        return ewma_vol_multiplier;
    }

} // namespace trade_ngin