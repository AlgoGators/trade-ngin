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
    trend_config_(std::move(trend_config)) { 

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
        std::cerr << "Base strategy initialization failed: "
            << base_result.error()->what() << std::endl;
        return base_result;
    }

    try {
        // Initialize price history containers for each symbol with proper capacity
        for (const auto& [symbol, _] : config_.trading_params) {
            price_history_[symbol].reserve(std::max(trend_config_.vol_lookback_long, 2520));
            volatility_history_[symbol].reserve(std::max(trend_config_.vol_lookback_long, 2520));
            
            // Initialize positions with zero quantity
            Position pos;
            pos.symbol = symbol;
            pos.quantity = 0.0;
            pos.average_price = 1.0;
            pos.last_update = std::chrono::system_clock::now();
            positions_[symbol] = pos;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        std::cerr << "Error in TrendFollowingStrategy::initialize: "
            << e.what() << std::endl;
        return make_error<void>(
            ErrorCode::STRATEGY_ERROR,
            std::string("Failed to initialize trend following strategy: ") + e.what(),
            "TrendFollowingStrategy"
        );
    }
}

Result<void> TrendFollowingStrategy::on_data(const std::vector<Bar>& data) {
    // Validate data
    if (data.empty()) {
        return Result<void>();
    }
    
    // Call base class data processing first
    auto base_result = BaseStrategy::on_data(data);
    if (base_result.is_error()) return base_result;

    // Get longest window in ema pairs
    int max_window = 0;
    for (const auto& window_pair : trend_config_.ema_windows) {
        max_window = std::max(max_window, window_pair.second);
    }
    
    try {
        // Group data by symbol and update price history
        for (const auto& bar : data) {
            // Validate essential fields
            if (bar.symbol.empty()) {
                return make_error<void>(
                    ErrorCode::INVALID_DATA,
                    "Bar has empty symbol",
                    "TrendFollowingStrategy"
                );
            }
            
            if (bar.timestamp == Timestamp{}) {
                return make_error<void>(
                    ErrorCode::INVALID_DATA,
                    "Bar has invalid timestamp",
                    "TrendFollowingStrategy"
                );
            }
            
            if (bar.open <= 0.0 || bar.high <= 0.0 || bar.high < bar.low || 
                bar.low <= 0.0 || bar.close <= 0.0 || bar.volume < 0.0) {
                    std::cout << "Invalid bar data for symbol " << bar.symbol << std::endl;
                    return make_error<void>(
                        ErrorCode::INVALID_DATA,
                        "Invalid bar data for symbol " + bar.symbol,
                        "TrendFollowingStrategy"
                    );
            }

            price_history_[bar.symbol].push_back(bar.close);
            
            // Wait for enough data before processing
            if (price_history_[bar.symbol].size() < max_window) {
                if (price_history_[bar.symbol].size() % 50 == 0) {
                    std::cout << "Waiting for enough data for symbol " << bar.symbol
                        << " (" << price_history_[bar.symbol].size() << " of " << max_window << ")" << std::endl;
                }
                INFO("Waiting for enough data for symbol " + bar.symbol
                + " (" + std::to_string(price_history_[bar.symbol].size()) + " of " 
                + std::to_string(max_window) + ")");
                continue;
            }
            
            // Get price history for the symbol
            const auto& prices = price_history_[bar.symbol];

            // Calculate volatility
            std::vector<double> volatility;
            try {
                volatility = blended_ewma_stddev(prices, trend_config_.vol_lookback_short);
                if (volatility.empty()) {
                    // If volatility calculation fails, use a default value
                    volatility.resize(prices.size(), 0.01);
                    INFO("Using default volatility for " + bar.symbol + " due to calculation issues");
                }
            } catch (const std::exception& e) {
                std::cout << "Volatility calculation exception for " << bar.symbol << ": " << e.what() << std::endl;
                INFO("Volatility calculation exception for " + bar.symbol + ": " + e.what());
                volatility.resize(prices.size(), 0.01);
            }
            
            // Save volatility history
            volatility_history_[bar.symbol] = volatility;

            // Get raw combined forecast
            std::vector<double> raw_forecasts;
            try {
                raw_forecasts = get_raw_combined_forecast(prices);
                if (raw_forecasts.empty()) {
                    INFO("Empty raw forecast for " + bar.symbol);

                    // Resize to avoid issues
                    raw_forecasts.resize(prices.size(), 0.0);
                }
            } catch (const std::exception& e) {
                std::cout << "Forecast calculation exception for " << bar.symbol << ": " << e.what() << std::endl;
                INFO("Forecast calculation exception for " + bar.symbol + ": " + e.what());

                // Resize to avoid issues
                raw_forecasts.resize(prices.size(), 0.0);
            }

             // Get scaled forecast
             std::vector<double> scaled_forecasts;
             try {
                scaled_forecasts = get_scaled_combined_forecast(raw_forecasts);
                if (scaled_forecasts.empty()) {
                    INFO("Empty scaled forecast for " + bar.symbol);
                    scaled_forecasts.resize(raw_forecasts.size(), 0.0);
                }
             } catch (const std::exception& e) {
                std::cout << "Scaled forecast exception for " << bar.symbol << ": " << e.what() << std::endl;
                INFO("Scaled forecast exception for " + bar.symbol + ": " + e.what());
                scaled_forecasts.resize(raw_forecasts.size(), 0.0);
             }

            // Calculate position using the most recent forecast value
            double raw_position = 0.0;
            double weight = trend_config_.weight;
            try {
                if (!scaled_forecasts.empty() && !volatility.empty()) {
                    double latest_forecast = scaled_forecasts.back();
                    double latest_volatility = volatility.back();
                    
                    // Guard against extreme values or NaN
                    if (std::isnan(latest_forecast) || std::isinf(latest_forecast)) {
                        INFO("Invalid forecast value for " + bar.symbol + ", using 0.0");

                        // Use 0.0 to avoid extreme positions
                        latest_forecast = 0.0;
                    }
                    
                    if (std::isnan(latest_volatility) || std::isinf(latest_volatility) || latest_volatility <= 0.0) {
                        INFO("Invalid volatility value for " + bar.symbol + ", using default 0.01");

                        // Use a default value to avoid extreme positions
                        latest_volatility = 0.2;
                    }
                    
                    raw_position = calculate_position(
                        bar.symbol,
                        latest_forecast,
                        weight,
                        bar.close,
                        latest_volatility
                    );
                }
            } catch (const std::exception& e) {
                std::cout << "Position calculation exception for " << bar.symbol << ": " << e.what() << std::endl;
                INFO("Position calculation exception for " + bar.symbol + ": " + e.what());
                raw_position = 0.0;
            }
            
            // Apply buffering if enabled with error handling
            double final_position = 0.0;
            try {
                if (trend_config_.use_position_buffering) {
                    final_position = apply_position_buffer(
                        bar.symbol, 
                        raw_position, 
                        bar.close, 
                        volatility.back()
                    );
                } else {
                    final_position = raw_position;
                }
                
                // Ensure position is reasonable (not NaN or infinite)
                if (std::isnan(final_position) || std::isinf(final_position)) {
                    INFO("Invalid final position for " + bar.symbol + ", using previous position");
                    
                    // Use previous position or zero
                    auto pos_it = positions_.find(bar.symbol);
                    final_position = (pos_it != positions_.end()) ? pos_it->second.quantity : 0.0;
                }                
            } catch (const std::exception& e) {
                std::cout << "Position buffering exception for " << bar.symbol << ": " << e.what() << std::endl;
                INFO("Position buffering exception for " + bar.symbol + ": " + e.what());
                
                // Use previous position or zero as fallback
                auto pos_it = positions_.find(bar.symbol);
                final_position = (pos_it != positions_.end()) ? pos_it->second.quantity : 0.0;
            }
            
            // Save forecast with error handling
            auto signal_result = on_signal(bar.symbol, scaled_forecasts.empty() ? 0.0 : scaled_forecasts.back());
            if (signal_result.is_error()) {
                WARN("Failed to save signal for " + bar.symbol + ": " + signal_result.error()->what());
                // Continue processing despite signal save failure
            }
            
            // Update position
            Position pos;
            pos.symbol = bar.symbol;
            pos.quantity = final_position;
            pos.average_price = bar.close;
            pos.last_update = bar.timestamp;
            
            auto pos_result = update_position(bar.symbol, pos);
            if (pos_result.is_error()) {
                WARN("Failed to update position for " + bar.symbol + ": " + pos_result.error()->what());
                // Continue processing despite position update failure
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

std::vector<double> TrendFollowingStrategy::ewma_standard_deviation(
    const std::vector<double>& prices, 
    int window) const {
    // Validation
    if (prices.empty() || window <= 0) {
        return std::vector<double>(1, 0.01);  // Return default value
    }
    
    if (prices.size() < 2) {
        return std::vector<double>(prices.size(), 0.01);  // Return default value
    }

    // Calculate returns with safety checks
    std::vector<double> returns(prices.size() - 1, 0.0);
    for (size_t i = 1; i < prices.size(); ++i) {
        // Avoid division by zero or negative prices
        if (prices[i - 1] <= 0.0 || prices[i] <= 0.0) {
            returns[i - 1] = 0.0;  // Use a neutral return
        } else {
            returns[i - 1] = std::log(prices[i] / prices[i - 1]);
            
            // Check for NaN or Inf
            if (std::isnan(returns[i - 1]) || std::isinf(returns[i - 1])) {
                returns[i - 1] = 0.0;  // Use a neutral return
            }
        }
    }

    std::vector<double> ewma_stddev(returns.size(), 0.0);
    std::vector<double> ewma_mean(returns.size(), 0.0);
    std::vector<double> ewma_variance(returns.size(), 0.0);

    double lambda = 2.0 / (window + 1); // Compute lambda
    ewma_mean[0] = returns[0];      // Initialize EWMA mean
    ewma_variance[0] = returns[0] * returns[0] * 0.1;        // Initial variance is zero

    for (size_t t = 1; t < returns.size(); ++t) {
        // Update EWMA mean
        ewma_mean[t] = lambda * returns[t] + (1 - lambda) * ewma_mean[t - 1];

        // Calculate deviation
        double deviation = returns[t] - ewma_mean[t];
        if (std::isnan(deviation) || std::isinf(deviation)) {
            deviation = 0.0;  // Use a neutral value
        }

        // Update EWMA variance
        ewma_variance[t] = lambda * (deviation * deviation) + (1 - lambda) * ewma_variance[t - 1];
        
        // Ensure variance is positive
        ewma_variance[t] = std::max(0.000001, ewma_variance[t]);
        
        ewma_stddev[t] = std::sqrt(ewma_variance[t]);
        
        // Final safety check - ensure stddev is positive
        if (ewma_stddev[t] <= 0.0 || std::isnan(ewma_stddev[t]) || std::isinf(ewma_stddev[t])) {
            ewma_stddev[t] = 0.001;  // Use a small, positive default
        } else if (ewma_stddev[t] > 0.5) {
            ewma_stddev[t] = 0.5;  // Cap at 50%
        }
    }

    // Handle first element
    ewma_stddev[0] = ewma_stddev[1];

    // Add a final element to match the size of the price vector if needed
    // (returns vector is one element shorter than prices)
    std::vector<double> result(prices.size(), 0.0);
    for (size_t i = 0; i < ewma_stddev.size(); ++i) {
        result[i + 1] = ewma_stddev[i];
    }
    result[0] = result[1]; // Copy first valid value

    return ewma_stddev;
}

double TrendFollowingStrategy::compute_long_term_avg(const std::vector<double>& history, size_t max_history) const {
    if (history.empty()) return 0.001;  // Return a small non-zero value instead of 0.0

    size_t start_index = history.size() > max_history ? history.size() - max_history : 0;
    double sum = std::accumulate(history.begin() + start_index, history.end(), 0.0);
    
    // Ensure we don't divide by zero
    size_t count = history.size() - start_index;
    if (count == 0) return 0.001;
    
    double result = sum / count;
    
    // Sanity check on the result
    if (std::isnan(result) || std::isinf(result) || result <= 0.0) {
        return 0.001;  // Return a safe default
    }
    
    return result;
}

std::vector<double> TrendFollowingStrategy::blended_ewma_stddev(
    const std::vector<double>& prices, 
    int window,
    double weight_short, 
    double weight_long,
    size_t max_history) const {

    if (prices.empty() || window <= 0) {
        std::cout << "Prices are empty or window is invalid" << std::endl;
        return std::vector<double>(1, 0.01);  // Return default value
    }

    if (prices.size() < 2) {
        std::cout << "WARNING: Not enough price data for stddev calculation" << std::endl;
        // Return a vector of small values
        return std::vector<double>(prices.size(), 0.01);
    }

    // Calculate EWMA standard deviation with error handling
    std::vector<double> ewma_stddev;
    try {
        ewma_stddev = ewma_standard_deviation(prices, window);
        if (ewma_stddev.empty()) {
            return std::vector<double>(prices.size(), 0.01);  // Default value
        }
    } catch (const std::exception& e) {
        std::cout << "Exception in ewma_standard_deviation: " << e.what() << std::endl;
        return std::vector<double>(prices.size(), 0.01);  // Default value
    }

    // Ensure ewma_stddev has the same size as prices
    if (ewma_stddev.size() != prices.size()) {
        // Adjust size by either copying the last value or truncating
        if (ewma_stddev.size() < prices.size()) {
            double last_value = ewma_stddev.empty() ? 0.01 : ewma_stddev.back();
            ewma_stddev.resize(prices.size(), last_value);
        } else {
            ewma_stddev.resize(prices.size());
        }
    }

    std::vector<double> blended_stddev(prices.size(), 0.0);
    std::vector<double> history; // Stores past short-term EWMA standard deviations

    // Apply floor to avoid division by zero or very small values
    const double MIN_STDDEV = 0.005;

    for (size_t t = 0; t < prices.size(); ++t) {
        // Ensure we have valid stddev value before pushing to history
        double valid_stddev = std::max(MIN_STDDEV, ewma_stddev[t]);
        if (!std::isnan(valid_stddev) && !std::isinf(valid_stddev)) {
            history.push_back(valid_stddev);  // Store the short-term EWMA standard deviation
        } else {
            history.push_back(MIN_STDDEV);  // Store a safe value
        }

        double long_term_avg = compute_long_term_avg(history, max_history);
        // Ensure long term average is also valid
        long_term_avg = std::max(MIN_STDDEV, long_term_avg);
        
        blended_stddev[t] = weight_short * valid_stddev + weight_long * long_term_avg;

        // Final safety check
        if (blended_stddev[t] <= 0.0 || std::isnan(blended_stddev[t]) || std::isinf(blended_stddev[t])) {
            blended_stddev[t] = MIN_STDDEV;  // Avoid division by zero
        }
    }

    return blended_stddev;
}

std::vector<double> TrendFollowingStrategy::get_raw_forecast(
    const std::vector<double>& prices,
    int short_window, 
    int long_window) const {

     // Validation
    if (prices.size() < std::max(short_window, long_window)) {
        std::cout << "Not enough price data for forecasts" << std::endl;
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Calculate EWMAs with error handling
    std::vector<double> short_ema;
    std::vector<double> long_ema;
    
    try {
        short_ema = calculate_ewma(prices, short_window);
        long_ema = calculate_ewma(prices, long_window);
    } catch (const std::exception& e) {
        std::cout << "Exception in calculate_ewma: " << e.what() << std::endl;
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }
    
    // Check if either EMA calculation failed
    if (short_ema.empty() || long_ema.empty()) {
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Get volatility with error handling
    std::vector<double> blended_stddev;
    double vol_multiplier = 1.0;  // Default value
    
    try {
        blended_stddev = blended_ewma_stddev(prices, trend_config_.vol_lookback_short);
        
        // Only calculate vol_multiplier if we have sufficient data
        if (prices.size() >= 252) {
            vol_multiplier = calculate_vol_regime_multiplier(prices, blended_stddev);
            // Ensure vol_multiplier is valid
            if (std::isnan(vol_multiplier) || std::isinf(vol_multiplier)) {
                vol_multiplier = 1.0;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "Exception in volatility calculation: " << e.what() << std::endl;
        // If volatility calculation fails, use a default value
        blended_stddev.resize(prices.size(), 0.01);
    }
    
    // Check volatility calculation
    if (blended_stddev.empty()) {
        blended_stddev.resize(prices.size(), 0.01);  // Use default value
    }

    // Generate forecast with safety checks
    std::vector<double> raw_forecast(prices.size(), 0.0);
    for (size_t i = 0; i < prices.size(); ++i) {
        // Safety check for accessing elements
        if (i < short_ema.size() && i < long_ema.size() && i < blended_stddev.size() && i < prices.size()) {
            // Ensure no division by zero
            double stddev_term = blended_stddev[i];
            if (stddev_term <= 0.0) stddev_term = 0.01;  // Minimum value
            
            double price_term = prices[i];
            if (price_term <= 0.0) price_term = 1.0;  // Minimum value
            
            raw_forecast[i] = (short_ema[i] - long_ema[i]) / (price_term * stddev_term / 16);
            
            // Apply regime multiplier
            raw_forecast[i] *= vol_multiplier;
        }
    }

    return raw_forecast;
}

std::vector<double> TrendFollowingStrategy::get_scaled_forecast(
    const std::vector<double>& raw_forecasts,
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

std::vector<double> TrendFollowingStrategy::get_raw_combined_forecast(
    const std::vector<double>& prices) const {
    if (prices.size() < 2) {
        std::cout << "Not enough price data for forecasts" << std::endl;
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }
    if (trend_config_.ema_windows.empty()) {
        std::cout << "No EMA windows defined" << std::endl;
        return std::vector<double>(prices.size(), 0.0);  // Return neutral forecast
    }

    // Initialize combined forecast and count valid window pairs
    std::vector<double> combined_forecast(prices.size(), 0.0);
    int valid_window_pairs = 0;

    // Iterate through each window pair
    for (const auto& window_pair : trend_config_.ema_windows) {
        try {
            // Calculate raw forecast for this window pair
            std::vector<double> raw_forecast = get_raw_forecast(
                prices, window_pair.first, window_pair.second);
            // Skip if invalid
            if (raw_forecast.empty() || raw_forecast.size() != prices.size()) {
                std::cout << "Invalid raw forecast for window pair (" 
                         << window_pair.first << ", " << window_pair.second 
                         << "), skipping" << std::endl;
                continue;
            }
            // Get volatility for scaling
            std::vector<double> blended_stddev;
            try {
                blended_stddev = blended_ewma_stddev(prices, window_pair.first);

                // Check if volatility calculation failed
                if (blended_stddev.empty() || blended_stddev.size() != prices.size()) {
                    std::cout << "Invalid volatility for window pair ("
                             << window_pair.first << ", " << window_pair.second
                             << "), using default" << std::endl;
                    blended_stddev.resize(prices.size(), 0.01);
                }
            } catch (const std::exception& e) {
                std::cout << "Exception in blended_ewma_stddev: " << e.what() << std::endl;
                blended_stddev.resize(prices.size(), 0.01);
            }
            // Get scaled forecast
            std::vector<double> scaled_forecast = get_scaled_forecast(raw_forecast, blended_stddev);

            // Combine forecasts
            for (size_t i = 0; i < combined_forecast.size(); ++i) {
                combined_forecast[i] += scaled_forecast[i];
            }
            valid_window_pairs++;
        } catch (const std::exception& e) {
            std::cout << "Exception in get_raw_combined_forecast: " << e.what() << std::endl;
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
    double weight,
    double price,
    double volatility) const {

    // Validation
    if (std::isnan(forecast)) {
        throw std::runtime_error("NaN forecast in position calculation for " + symbol);
    }

    if (std::isnan(weight) || weight <= 0.0) {
        throw std::runtime_error("Invalid price in position calculation for " + symbol + ": " + std::to_string(price));
    }
    
    if (std::isnan(price) || price <= 0.0) {
        throw std::runtime_error("Invalid price in position calculation for " + symbol + ": " + std::to_string(price));
    }
    
    if (std::isnan(volatility) || volatility <= 0.0) {
        throw std::runtime_error("Invalid volatility in position calculation for " + symbol + ": " + std::to_string(volatility));
    }
    
    // Get contract specification with robust error handling
    const auto& contracts = config_.trading_params;
    double contract_size = 1.0;  // Default value
    
    if (contracts.count(symbol) > 0) {
        contract_size = contracts.at(symbol);
    } else {
        std::cout << "WARNING: No contract specification for " << symbol << ", using default size" << std::endl;
    }
    
    // Ensure all parameters are valid
    double capital = std::max(1000.0, config_.capital_allocation);
    double idm = std::max(0.1, trend_config_.idm);
    double risk_target = std::max(0.01, std::min(0.5, trend_config_.risk_target));
    double fx_rate = std::max(0.1, trend_config_.fx_rate);
    
    // Cap forecast to reasonable values
    forecast = std::max(-20.0, std::min(20.0, forecast));
    
    // Apply minimum value to volatility to avoid division by very small values
    volatility = std::max(0.01, volatility);
    
    // Calculate position using volatility targeting formula with safeguards
    double denominator = 10.0 * contract_size * price * fx_rate * volatility;
    if (denominator <= 0.0) {
        denominator = 1.0;  // Prevent division by zero
    }
    
    double position = (forecast * capital * weight * idm * risk_target) / denominator;
    
    // Handle potential NaN or Inf results
    if (std::isnan(position) || std::isinf(position)) {
        position = 0.0;  // Use neutral position
    }
    
    // Round position to nearest integer
    return std::round(position);
}

double TrendFollowingStrategy::apply_position_buffer(
    const std::string& symbol,
    double raw_position,
    double price,
    double volatility) const {

    if (!trend_config_.use_position_buffering) {
        return raw_position;
    }
    
    // Validation
    if (std::isnan(raw_position) || std::isinf(raw_position)) {
        std::cout << "Invalid raw position for " << symbol << ": " << raw_position << std::endl;
        return 0.0;
    }

    if (std::isnan(price) || price <= 0.0) {
        std::cout << "Invalid price for " << symbol << ": " << price << std::endl;
        return 0.0;
    }

    if (std::isnan(volatility) || std::isinf(volatility) || volatility <= 0.0) {
        std::cout << "Invalid volatility for " << symbol << ": " << volatility << std::endl;
        return 0.0;
    }

    // Get current position with safeguards
    double current_position = 0.0;
    auto pos_it = positions_.find(symbol);
    if (pos_it != positions_.end()) {
        current_position = pos_it->second.quantity;
        
        // Sanity check on current position
        if (std::isnan(current_position) || std::isinf(current_position)) {
            current_position = 0.0;
        }
    }

    double weight = std::max(0.0, trend_config_.weight);
    double contract_size = config_.trading_params.count(symbol) > 0 ? config_.trading_params.at(symbol) : 1.0;

    // Calculate buffer width as:
    // 0.1 x capital x IDM x weight x tau /
    // (contract size x price x fx rate x volatility)
    double buffer_width = 0.1 * config_.capital_allocation * trend_config_.idm  * trend_config_.risk_target * weight /
        (contract_size * price * trend_config_.fx_rate * volatility);

    // Calculate buffer bounds
    double lower_buffer = raw_position - buffer_width;
    double upper_buffer = raw_position + buffer_width;

    // Apply buffering logic
    double new_position;
    if (current_position < lower_buffer) {
        new_position = std::round(lower_buffer);
    } else if (current_position > upper_buffer) {
        new_position = std::round(upper_buffer);
    } else {
        new_position = std::round(current_position);
    }

    // Debug large changes that are being dampened
    if (std::abs(raw_position) > 50 && std::abs(new_position) < 10) {
        std::cout << "DEBUG: Buffer dampening large position: raw=" 
                 << raw_position << ", buffered=" << new_position 
                 << ", current=" << current_position << std::endl;
    }
    
    // Final safety check - cap positions to a reasonable maximum for testing
    double position_limit = 1000.0;
    if (config_.position_limits.count(symbol) > 0) {
        position_limit = config_.position_limits.at(symbol);
    }
    
    return std::max(-position_limit, std::min(position_limit, new_position));
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