#pragma once
#include "strategy.hpp"
#include "signals.hpp"
#include "../data/dataframe.hpp"
#include <memory>
#include <unordered_map>

class TrendStrategy : public Strategy {
public:
    TrendStrategy() {}  // Default constructor for flexibility

    void configureSignals(
        const std::unordered_map<std::string, double>& ma_params,
        const std::unordered_map<std::string, double>& vol_params,
        const std::unordered_map<std::string, double>& regime_params,
        const std::unordered_map<std::string, double>& momentum_params,
        const std::unordered_map<std::string, double>& weight_params) {
            
        ma_params_ = ma_params;
        vol_params_ = vol_params;
        regime_params_ = regime_params;
        momentum_params_ = momentum_params;
        weight_params_ = weight_params;
    }

    std::vector<double> generateSignals(const std::vector<MarketData>& market_data) {
        if (market_data.empty()) return {};

        // Extract close prices
        std::vector<double> prices;
        prices.reserve(market_data.size());
        for (const auto& bar : market_data) {
            prices.push_back(bar.close);
        }

        // Calculate returns and volatility
        auto returns = calculateReturns(prices);
        auto vol = calculateVolatility(returns, vol_params_["window"]);
        
        // Calculate average volatility for adaptive thresholds
        double avg_vol = 0.0;
        for (size_t i = std::max(0, (int)vol.size() - 20); i < vol.size(); ++i) {
            avg_vol += vol[i];
        }
        avg_vol /= std::min(20, (int)vol.size());
        
        // Generate trend signals for each timeframe
        double combined_signal = 0.0;
        double trend_strength = 0.0;
        
        // Short-term signals (60% weight total)
        std::vector<double> short_signals;
        for (int i = 1; i <= 6; ++i) {
            auto ma = calculateMA(prices, ma_params_["short_window_" + std::to_string(i)]);
            auto signal = generateTrendSignal(ma, prices);
            
            // Calculate trend strength based on price distance from MA
            double distance = std::abs(prices.back() - ma.back()) / (avg_vol * prices.back());
            trend_strength += distance;
            
            short_signals.push_back(signal.back());
            combined_signal += signal.back() * weight_params_["short_weight"] * 0.6;  // Reduced from 0.7
        }
        
        // Long-term signals (40% weight total)
        std::vector<double> long_signals;
        for (int i = 1; i <= 3; ++i) {
            auto ma = calculateMA(prices, ma_params_["long_window_" + std::to_string(i)]);
            auto signal = generateTrendSignal(ma, prices);
            
            double distance = std::abs(prices.back() - ma.back()) / (avg_vol * prices.back());
            trend_strength += distance;
            
            long_signals.push_back(signal.back());
            combined_signal += signal.back() * weight_params_["long_weight"] * 0.4;  // Increased from 0.3
        }

        // Normalize trend strength
        trend_strength /= 9.0;  // Total number of signals

        // Add momentum component with reduced adaptive boost
        auto momentum = calculateMomentum(prices, momentum_params_["lookback"]);
        double momentum_boost = 0.15 * (1.0 + trend_strength);  // Reduced from 0.2
        combined_signal *= (1.0 + momentum_boost * std::copysign(1.0, momentum.back()));

        // Calculate signal agreement with smoother scaling
        int agreement = 0;
        for (auto sig : short_signals) agreement += (sig * combined_signal > 0) ? 1 : 0;
        for (auto sig : long_signals) agreement += (sig * combined_signal > 0) ? 1 : 0;
        double agreement_ratio = 0.5 + (agreement / 18.0);  // Base 0.5 scaling plus agreement bonus

        // Apply volatility regime with less aggressive thresholds
        double vol_scalar = 1.0;
        double current_vol = vol.back();
        double high_thresh = avg_vol * vol_params_["high_vol_threshold"] * 1.2;  // 20% higher threshold
        double low_thresh = avg_vol * vol_params_["low_vol_threshold"] * 0.8;   // 20% lower threshold

        if (current_vol > high_thresh) {
            vol_scalar = 0.7 * agreement_ratio;  // Less reduction in high vol
        } else if (current_vol < low_thresh) {
            vol_scalar = 1.3 * agreement_ratio;  // Less increase in low vol
        } else {
            vol_scalar = 1.0 * agreement_ratio;
        }

        // Generate final signals
        std::vector<double> signals(market_data.size(), 0.0);
        signals.back() = combined_signal * vol_scalar;
            
        // Ensure signal is between -1 and 1
        signals.back() = std::max(std::min(signals.back(), 1.0), -1.0);

        return signals;
    }

private:
    std::unordered_map<std::string, double> ma_params_;
    std::unordered_map<std::string, double> vol_params_;
    std::unordered_map<std::string, double> regime_params_;
    std::unordered_map<std::string, double> momentum_params_;
    std::unordered_map<std::string, double> weight_params_;

    std::vector<double> calculateReturns(const std::vector<double>& prices) {
        std::vector<double> returns;
        returns.reserve(prices.size() - 1);
        for (size_t i = 1; i < prices.size(); ++i) {
            returns.push_back((prices[i] / prices[i-1]) - 1.0);
        }
        return returns;
    }

    std::vector<double> calculateMA(const std::vector<double>& data, int window) {
        std::vector<double> ma(data.size());
        for (size_t i = window - 1; i < data.size(); ++i) {
            double sum = 0;
            for (int j = 0; j < window; ++j) {
                sum += data[i - j];
            }
            ma[i] = sum / window;
        }
        return ma;
    }

    std::vector<double> calculateVolatility(const std::vector<double>& returns, int window) {
        std::vector<double> vol(returns.size());
        for (size_t i = window - 1; i < returns.size(); ++i) {
            double sum_sq = 0;
            for (int j = 0; j < window; ++j) {
                sum_sq += returns[i - j] * returns[i - j];
            }
            vol[i] = std::sqrt(sum_sq / window) * std::sqrt(252.0); // Annualized
        }
        return vol;
    }

    std::vector<double> generateTrendSignal(const std::vector<double>& ma, const std::vector<double>& prices) {
        std::vector<double> signal(prices.size(), 0.0);
        for (size_t i = 1; i < signal.size(); ++i) {
            if (prices[i] > ma[i]) {
                signal[i] = 1.0;
            } else if (prices[i] < ma[i]) {
                signal[i] = -1.0;
            }
        }
        return signal;
    }

    std::vector<double> calculateMomentum(const std::vector<double>& prices, double lookback) {
        std::vector<double> momentum(prices.size(), 0.0);
        for (size_t i = lookback; i < prices.size(); ++i) {
            momentum[i] = (prices[i] / prices[i - (int)lookback]) - 1.0;
        }
        return momentum;
    }
}; 