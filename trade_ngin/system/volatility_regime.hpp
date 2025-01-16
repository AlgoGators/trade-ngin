#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

class VolatilityRegime {
public:
    struct RegimeConfig {
        int ewma_span = 10;     // EWMA span for smoothing
        double scalar_low = 2.0; // Scalar for low volatility (Q = 0.0)
        double scalar_high = 0.5;// Scalar for high volatility (Q = 1.0)
    };

    VolatilityRegime(const RegimeConfig& config = RegimeConfig()) 
        : config_(config) {}

    // Calculate the quantile point Q for a given volatility value
    double calculateQuantilePoint(double current_vol, const std::vector<double>& historical_vol) {
        if (historical_vol.empty()) return 0.5;  // Default to median if no history
        
        // Count how many historical values are less than current
        int count = std::count_if(historical_vol.begin(), historical_vol.end(),
                                [current_vol](double v) { return v < current_vol; });
        
        return static_cast<double>(count) / historical_vol.size();
    }

    // Calculate EWMA with specified span
    double calculateEWMA(const std::vector<double>& values, int span) {
        if (values.empty()) return 0.0;
        
        double alpha = 2.0 / (span + 1.0);
        double ewma = values[0];
        
        for (size_t i = 1; i < values.size(); ++i) {
            ewma = alpha * values[i] + (1.0 - alpha) * ewma;
        }
        
        return ewma;
    }

    // Calculate volatility multiplier M as per the book
    double calculateVolMultiplier(double current_vol, const std::vector<double>& historical_vol) {
        // Calculate quantile point
        double Q = calculateQuantilePoint(current_vol, historical_vol);
        
        // Calculate raw multiplier: M = 2 - 1.5 × Q
        double raw_multiplier = 2.0 - 1.5 * Q;
        
        // Apply EWMA smoothing with 10-day span
        std::vector<double> multiplier_series = {raw_multiplier};
        return calculateEWMA(multiplier_series, config_.ewma_span);
    }

    // Adjust trend forecast
    double adjustTrendForecast(double raw_forecast, double multiplier) {
        return raw_forecast * multiplier;
    }

    // Adjust carry forecast (with reduced scalar)
    double adjustCarryForecast(double smoothed_carry_forecast, double multiplier) {
        return smoothed_carry_forecast * multiplier;
    }

    // Get forecast scalar based on type (trend vs carry)
    double getForecastScalar(bool is_carry) {
        return is_carry ? 23.0 : 30.0;  // As mentioned in the book
    }

private:
    RegimeConfig config_;
}; 