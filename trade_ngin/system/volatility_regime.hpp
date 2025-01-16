#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

class VolatilityRegime {
public:
    enum class Regime {
        LOW_VOLATILITY,
        NORMAL_VOLATILITY,
        HIGH_VOLATILITY
    };

    struct RegimeConfig {
        int fast_window = 20;      // Fast volatility window
        int slow_window = 60;      // Slow volatility window
        double high_threshold = 1.5;// High vol threshold multiplier
        double low_threshold = 0.5; // Low vol threshold multiplier
        bool use_relative = true;  // Use relative or absolute thresholds
    };

    VolatilityRegime(const RegimeConfig& config = RegimeConfig()) 
        : config_(config) {}

    // Detect regime using fast/slow volatility comparison
    Regime detectRegime(const std::vector<double>& returns) {
        if (returns.size() < std::max(config_.fast_window, config_.slow_window)) {
            return Regime::NORMAL_VOLATILITY;
        }

        // Calculate fast and slow volatility
        double fast_vol = calculateVolatility(returns, config_.fast_window);
        double slow_vol = calculateVolatility(returns, config_.slow_window);

        if (config_.use_relative) {
            // Relative regime detection (fast vol compared to slow vol)
            if (fast_vol > slow_vol * config_.high_threshold) {
                return Regime::HIGH_VOLATILITY;
            } else if (fast_vol < slow_vol * config_.low_threshold) {
                return Regime::LOW_VOLATILITY;
            }
        } else {
            // Absolute regime detection (compared to historical average)
            double avg_vol = calculateAverageVolatility(returns, config_.slow_window);
            if (fast_vol > avg_vol * config_.high_threshold) {
                return Regime::HIGH_VOLATILITY;
            } else if (fast_vol < avg_vol * config_.low_threshold) {
                return Regime::LOW_VOLATILITY;
            }
        }

        return Regime::NORMAL_VOLATILITY;
    }

    // Get regime scaling factor for position sizing
    double getRegimeScalar(const std::vector<double>& returns) {
        Regime regime = detectRegime(returns);
        switch (regime) {
            case Regime::HIGH_VOLATILITY:
                return 0.7;  // Reduce position size in high vol
            case Regime::LOW_VOLATILITY:
                return 1.3;  // Increase position size in low vol
            default:
                return 1.0;  // Normal position size
        }
    }

    // Calculate rolling volatility
    std::vector<double> calculateRollingVol(const std::vector<double>& returns, int window) {
        std::vector<double> vol(returns.size());
        for (size_t i = window - 1; i < returns.size(); ++i) {
            vol[i] = calculateVolatility(
                std::vector<double>(returns.begin() + (i - window + 1), returns.begin() + i + 1),
                window
            );
        }
        return vol;
    }

private:
    RegimeConfig config_;

    // Calculate volatility for a window of returns
    double calculateVolatility(const std::vector<double>& returns, int window) {
        if (returns.size() < window) return 0.0;

        // Use the last 'window' returns
        auto start = returns.end() - window;
        auto end = returns.end();

        double sum = 0.0;
        double sum_sq = 0.0;
        
        for (auto it = start; it != end; ++it) {
            sum += *it;
            sum_sq += (*it) * (*it);
        }

        double mean = sum / window;
        double variance = (sum_sq / window) - (mean * mean);
        
        return std::sqrt(variance * 252.0); // Annualized
    }

    // Calculate average volatility over a period
    double calculateAverageVolatility(const std::vector<double>& returns, int window) {
        auto vol = calculateRollingVol(returns, window);
        
        // Average the non-zero volatilities
        double sum = 0.0;
        int count = 0;
        for (double v : vol) {
            if (v > 0) {
                sum += v;
                count++;
            }
        }
        
        return count > 0 ? sum / count : 0.0;
    }
}; 