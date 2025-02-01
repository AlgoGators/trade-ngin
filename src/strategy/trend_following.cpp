// src/strategy/trend_following.cpp
#include "trade_ngin/strategy/trend_following.hpp"
#include <cmath>
#include <numeric>
#include <algorithm>

namespace trade_ngin {

TrendFollowingStrategy::TrendFollowingStrategy(
    std::string id,
    StrategyConfig config,
    TrendFollowingConfig trend_config,
    std::shared_ptr<DatabaseInterface> db)
    : BaseStrategy(std::move(id), std::move(config), std::move(db))
    , trend_config_(std::move(trend_config)) {
    
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

std::vector<double> TrendFollowingStrategy::calculate_ema_crossover(
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
    
    // Calculate crossover signals
    std::vector<double> signals(prices.size());
    for (size_t i = 0; i < prices.size(); ++i) {
        signals[i] = (short_ema[i] - long_ema[i]) / (prices[i] * std::sqrt(long_window) / 16.0);
    }
    
    return signals;
}

std::vector<double> TrendFollowingStrategy::calculate_volatility(
    const std::vector<double>& prices,
    int short_lookback,
    int long_lookback) const {
    
    std::vector<double> returns(prices.size() - 1);
    for (size_t i = 1; i < prices.size(); ++i) {
        returns[i-1] = std::log(prices[i] / prices[i-1]);
    }
    
    std::vector<double> volatility(prices.size(), 0.0);
    
    // Calculate rolling standard deviations
    for (size_t i = std::max(short_lookback, long_lookback); i < prices.size(); ++i) {
        // Short-term volatility
        double short_var = 0.0;
        for (int j = 0; j < short_lookback; ++j) {
            double ret = returns[i-j-1];
            short_var += ret * ret;
        }
        short_var /= (short_lookback - 1);
        
        // Long-term volatility
        double long_var = 0.0;
        for (int j = 0; j < long_lookback; ++j) {
            double ret = returns[i-j-1];
            long_var += ret * ret;
        }
        long_var /= (long_lookback - 1);
        
        // Blend volatilities
        volatility[i] = std::sqrt(0.5 * short_var + 0.5 * long_var) * std::sqrt(252.0);
    }
    
    return volatility;
}

std::vector<double> TrendFollowingStrategy::generate_raw_forecasts(
    const std::vector<double>& prices,
    const std::vector<double>& volatility) const {
    
    std::vector<std::vector<double>> all_signals;
    
    // Calculate signals for each EMA pair
    for (const auto& window_pair : trend_config_.ema_windows) {
        auto signals = calculate_ema_crossover(
            prices,
            window_pair.first,
            window_pair.second
        );
        all_signals.push_back(signals);
    }
    
    // Combine signals with equal weighting
    std::vector<double> combined_forecast(prices.size(), 0.0);
    for (size_t i = 0; i < prices.size(); ++i) {
        double sum = 0.0;
        int count = 0;
        
        for (const auto& signals : all_signals) {
            if (i < signals.size() && !std::isnan(signals[i])) {
                sum += signals[i];
                ++count;
            }
        }
        
        if (count > 0) {
            // Scale by 1.26 and cap between -20 and 20
            double forecast = (sum / count) * 1.26;
            combined_forecast[i] = std::max(-20.0, std::min(20.0, forecast));
        }
    }
    
    return combined_forecast;
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

} // namespace trade_ngin