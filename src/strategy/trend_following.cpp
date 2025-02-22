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

    // Verify lengths of lookback periods
    if (trend_config_.vol_lookback_short <= 0) {
        trend_config_.vol_lookback_short = 22;
    }
    if (trend_config_.vol_lookback_long <= trend_config_.vol_lookback_short) {
        trend_config_.vol_lookback_long = trend_config_.vol_lookback_short * 4;
    }
    
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

        // Initialize positions for each symbol
        for (const auto& symbol_param : config_.trading_params) {
            Position pos;
            pos.symbol = symbol_param.first;
            pos.quantity = 0.0;
            pos.average_price = 0.0;
            pos.last_update = std::chrono::system_clock::now();
            positions_[symbol_param.first] = pos;
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
    // Call base class data processing first
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error()) return base_result;

    // Add validation for bar fields
    for (const auto& bar : data) {
        if (bar.high <= 0 || bar.low <= 0 || bar.open <= 0 || bar.volume <= 0) {
            return make_error<void>(
                ErrorCode::INVALID_DATA,
                "Invalid bar data for symbol " + bar.symbol,
                "TrendFollowingStrategy"
            );
        }
    }

    // Group data by symbol before processing
    std::unordered_map<std::string, std::vector<Bar>> grouped_data;
    for (const auto& bar : data) {
        grouped_data[bar.symbol].push_back(bar);
    }
    
    try {
        // Validate data
        if (data.empty()) {
            return Result<void>();
        }

        // Get longest window in ema pairs
        int max_window = 0;
        for (const auto& window_pair : trend_config_.ema_windows) {
            max_window = std::max(max_window, window_pair.second);
        }

        // Group data by symbol and update price history
        for (const auto& bar : data) {
            // Validate essential fields
            if (bar.symbol.empty() || bar.close <= 0.0) {
                return make_error<void>(
                    ErrorCode::INVALID_DATA,
                    "Invalid bar data",
                    "TrendFollowingStrategy"
                );
            }
            price_history_[bar.symbol].push_back(bar.close);
            
            // Wait for enough data before processing
            if (price_history_[bar.symbol].size() < max_window) {
                INFO("Accumulating data for " + bar.symbol + ": " + 
                     std::to_string(price_history_[bar.symbol].size()) + "/" +
                     std::to_string(max_window));
                continue;
            }
            
            const auto& prices = price_history_[bar.symbol];

            auto volatility = blended_ewma_stddev(prices, trend_config_.vol_lookback_short);
            volatility_history_[bar.symbol] = volatility;

            // Get raw combined forecast
            auto raw_forecasts = get_raw_combined_forecast(prices);
            auto scaled_forecasts = get_scaled_combined_forecast(raw_forecasts);
            
            // Calculate position using the most recent forecast value
            double raw_position = calculate_position(
                bar.symbol,
                scaled_forecasts.back(), // Use latest forecast
                bar.close,
                volatility.back()
            );
            
            // Apply buffering if enabled
            double final_position = trend_config_.use_position_buffering
                ? apply_position_buffer(bar.symbol, raw_position, bar.close, volatility.back())
                : raw_position;
            
            // Save forecast
            auto signal_result = on_signal(bar.symbol, scaled_forecasts.back());
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
        
        return Result<void>();
        
    } catch (const std::exception& e) {
        std::cout << "Error in TrendFollowingStrategy::on_data: " << std::string(e.what());
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Error processing data: ") + e.what(),
            "TrendFollowingStrategy"
        );
    }
}

std::vector<double> TrendFollowingStrategy::calculate_ewma(
    const std::vector<double>& prices,
    int window) const {

    std::vector<double> ewma(prices.size(), 0.0);
    if (prices.empty() || window <= 0) {
        return ewma;
        }
    double lambda = 2.0 / (window + 1);
    ewma[0] = prices[0];

    for (size_t i = 1; i < prices.size(); ++i) {
        ewma[i] = lambda * prices[i] + (1 - lambda) * ewma[i - 1];
    }
    return ewma;
}

std::vector<double> TrendFollowingStrategy::ewma_standard_deviation(const std::vector<double>& prices, int window) const {
    if (prices.empty() || window <= 0) return {};

    std::vector<double> ewma_stddev(prices.size(), 0.0);
    std::vector<double> ewma_mean(prices.size(), 0.0);
    std::vector<double> ewma_variance(prices.size(), 0.0);

    double lambda = 2.0 / (window + 1); // Compute lambda
    ewma_mean[0] = prices[0];      // Initialize EWMA mean
    ewma_variance[0] = 0.0;        // Initial variance is zero

    for (size_t t = 1; t < prices.size(); ++t) {
        ewma_mean[t] = lambda * prices[t] + (1 - lambda) * ewma_mean[t - 1];
        double deviation = prices[t] - ewma_mean[t];
        ewma_variance[t] = lambda * (deviation * deviation) + (1 - lambda) * ewma_variance[t - 1];
        ewma_stddev[t] = std::sqrt(ewma_variance[t]);
    }

    return ewma_stddev;
}

double TrendFollowingStrategy::compute_long_term_avg(const std::vector<double>& history, size_t max_history) const {
    if (history.empty()) return 0.0;

    size_t start_index = history.size() > max_history ? history.size() - max_history : 0;
    double sum = std::accumulate(history.begin() + start_index, history.end(), 0.0);
    return sum / (history.size() - start_index);
}

std::vector<double> TrendFollowingStrategy::blended_ewma_stddev(const std::vector<double>& prices, int window,
double weight_short, double weight_long,
size_t max_history) const {
    if (prices.empty() || window <= 0) return {};

    std::vector<double> ewma_stddev = ewma_standard_deviation(prices, window);
    std::vector<double> blended_stddev(prices.size(), 0.0);
    std::vector<double> history; // Stores past short-term EWMA standard deviations

    for (size_t t = 0; t < prices.size(); ++t) {
        history.push_back(ewma_stddev[t]);  // Store the short-term EWMA standard deviation

        double long_term_avg = compute_long_term_avg(history, max_history);
        blended_stddev[t] = weight_short * ewma_stddev[t] + weight_long * long_term_avg;

        if (blended_stddev[t] <= 0.0 || std::isnan(blended_stddev[t])) {
            blended_stddev[t] = 0.001;  // Avoid division by zero
        }
    }

    return blended_stddev;
}

std::vector<double> TrendFollowingStrategy::get_raw_forecast(const std::vector<double>& prices, int short_window, int long_window) const {
    if (prices.size() < 2) return {};

    std::vector<double> short_ema = calculate_ewma(prices, short_window);
    std::vector<double> long_ema = calculate_ewma(prices, long_window);

    std::vector<double> blended_stddev = blended_ewma_stddev(prices, short_window);
    double vol_multiplier = calculate_vol_regime_multiplier(prices, blended_stddev);

    std::vector<double> raw_forecast = std::vector<double>(prices.size(), 0.0);
    for (size_t i = 0; i < prices.size(); ++i) {
        raw_forecast[i] = (short_ema[i] - long_ema[i]) / blended_stddev[i];
        raw_forecast[i] *= vol_multiplier;
    }

    return raw_forecast;
}


std::vector<double> TrendFollowingStrategy::get_scaled_forecast(const std::vector<double>& raw_forecasts,
    const std::vector<double>& blended_stddev) const {
    if (raw_forecasts.empty() || blended_stddev.empty()) return {};

    double abs_sum = get_abs_value(raw_forecasts);
    double abs_avg = abs_sum / raw_forecasts.size();

    std::vector<double> scaled_forecasts(raw_forecasts.size(), 0.0);
    for (size_t i = 0; i < raw_forecasts.size(); ++i) {
        scaled_forecasts[i] = 10.0 * raw_forecasts[i] / abs_avg;
        scaled_forecasts[i] = std::max(-20.0, std::min(20.0, scaled_forecasts[i]));
    }

    return scaled_forecasts;
}        

std::vector<double> TrendFollowingStrategy::get_raw_combined_forecast(const std::vector<double>& prices) const {
    if (prices.size() < 2) return {};

    std::vector<double> combined_forecast(prices.size(), 0.0);
    int valid_window_pairs = 0;

    for (const auto& window_pair : trend_config_.ema_windows) {
        std::vector<double> raw_forecast = get_raw_forecast(prices, window_pair.first, window_pair.second);

        // Skip invalid forecasts
        if (raw_forecast.empty()) continue;

        std::vector<double> blended_stddev = blended_ewma_stddev(prices, window_pair.first);
        std::vector<double> scaled_forecast = get_scaled_forecast(raw_forecast, blended_stddev);

        if (scaled_forecast.empty()) continue;

        valid_window_pairs++;
        
        for (size_t i = 0; i < prices.size(); ++i) {
            combined_forecast[i] += scaled_forecast[i];
        }        
    }

    // Normalize combined forecast by the number of valid window pairs
    if (valid_window_pairs > 0) {
        for (size_t i = 0; i < combined_forecast.size(); ++i) {
            combined_forecast[i] /= valid_window_pairs;
        }
    } else {
        combined_forecast.clear();
    }
    
    return combined_forecast;
}

std::vector<double> TrendFollowingStrategy::get_scaled_combined_forecast(const std::vector<double>& raw_combined_forecast) const {
    if (raw_combined_forecast.empty()) return {};

    // Get FDM from trend_config_ based on the number of rules
    size_t num_rules = trend_config_.ema_windows.size();
    double fdm = 1.0; // Default if not found
    for (const auto& fdm_pair : trend_config_.fdm) {
        if (fdm_pair.first == num_rules) {
            fdm = fdm_pair.second;
            break;
        }
    }

    // Multiply raw combined forecast by FDM and scale to [-20, 20]
    std::vector<double> scaled_combined_forecast(raw_combined_forecast.size(), 0.0);
    for (size_t i = 0; i < raw_combined_forecast.size(); ++i) {
        scaled_combined_forecast[i] = raw_combined_forecast[i] * fdm;
        scaled_combined_forecast[i] = std::max(-20.0, std::min(20.0, scaled_combined_forecast[i]));
    }

    return scaled_combined_forecast;
}


double TrendFollowingStrategy::get_abs_value(const std::vector<double>& values) const {
    return std::accumulate(values.begin(), values.end(), 0.0,
        [](double acc, double val) { return acc + std::abs(val); });
}

double TrendFollowingStrategy::calculate_position(
    const std::string& symbol,
    double forecast,
    double price,
    double volatility) const {

    // Validate inputs
    if (std::isnan(forecast) || std::isnan(price) || std::isnan(volatility)) {
        throw std::runtime_error("NaN values in position calculation");
    }
    
    if (price <= 0.0 || volatility <= 0.0) {
        throw std::runtime_error("Invalid price or volatility in position calculation");
    }
    
    // Get contract specification
    const auto& contracts = config_.trading_params;
    if (contracts.count(symbol) == 0) {
        throw std::runtime_error("No contract specification for symbol: " + symbol);
    }
    double contract_size = contracts.at(symbol);
    
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

    if (!trend_config_.use_position_buffering) {
        return raw_position;
    }
    
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

        // Calculate lookback period for long-run average
        size_t max_lookback = 2520;  // 10 years
        size_t available_days = prices.size();
        size_t lookback = std::min(available_days, max_lookback);

        // Calculate long-run average volatility
        double sum_vol = 0.0;
        size_t count = 0;
        for (size_t i = volatility.size() - lookback; i < volatility.size(); ++i) {
            sum_vol += volatility[i];
            count++;
        }
        double avg_vol = sum_vol / count;

        // Calculate relative volatility level
        double relative_vol_level = current_vol / avg_vol;

        // Calculate quantile of relative volatility levels over lookback period
        std::vector<double> historical_rel_vol_levels;
        historical_rel_vol_levels.reserve(lookback);
        
        for (size_t i = volatility.size() - lookback; i < volatility.size() - 1; ++i) {  // Exclude current day
            historical_rel_vol_levels.push_back(volatility[i] / avg_vol);
        }

        // Sort to calculate quantile
        std::sort(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end());
        
        // Find position of current relative volatility level in sorted historical values
        auto it = std::upper_bound(historical_rel_vol_levels.begin(), historical_rel_vol_levels.end(), relative_vol_level);
        double quantile = static_cast<double>(std::distance(historical_rel_vol_levels.begin(), it)) / 
                static_cast<double>(historical_rel_vol_levels.size());

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