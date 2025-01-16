#pragma once
#include "strategy.hpp"
#include "signals.hpp"
#include "../data/dataframe.hpp"
#include <memory>
#include <unordered_map>

class MeanReversionStrategy : public Strategy {
public:
    MeanReversionStrategy() {}  // Default constructor

    void configureSignals(
        const std::unordered_map<std::string, double>& ma_params,
        const std::unordered_map<std::string, double>& vol_params,
        const std::unordered_map<std::string, double>& zscore_params,
        const std::unordered_map<std::string, double>& weight_params) {
            
        ma_params_ = ma_params;
        vol_params_ = vol_params;
        zscore_params_ = zscore_params;
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

        // Calculate moving average and standard deviation
        auto ma = calculateMA(prices, ma_params_["window"]);
        auto std_dev = calculateStdDev(prices, ma, ma_params_["window"]);
        
        // Calculate z-score (deviation from mean in standard deviation units)
        std::vector<double> signals(prices.size(), 0.0);
        for (size_t i = ma_params_["window"]; i < prices.size(); ++i) {
            if (std_dev[i] > 0) {
                double z_score = (prices[i] - ma[i]) / std_dev[i];
                
                // Generate mean reversion signals based on z-score thresholds
                if (z_score > zscore_params_["upper_threshold"]) {
                    signals[i] = -1.0;  // Price too high, go short
                } else if (z_score < zscore_params_["lower_threshold"]) {
                    signals[i] = 1.0;   // Price too low, go long
                }
                
                // Scale signal by volatility
                double vol_scalar = calculateVolScalar(std_dev[i], vol_params_);
                signals[i] *= vol_scalar;
                
                // Scale by confidence (higher z-score = stronger signal)
                double confidence = std::min(std::abs(z_score) / zscore_params_["max_zscore"], 1.0);
                signals[i] *= confidence * weight_params_["base_size"];
            }
        }

        return signals;
    }

private:
    std::unordered_map<std::string, double> ma_params_;
    std::unordered_map<std::string, double> vol_params_;
    std::unordered_map<std::string, double> zscore_params_;
    std::unordered_map<std::string, double> weight_params_;

    std::vector<double> calculateMA(const std::vector<double>& data, int window) {
        std::vector<double> ma(data.size(), 0.0);
        for (size_t i = window - 1; i < data.size(); ++i) {
            double sum = 0;
            for (int j = 0; j < window; ++j) {
                sum += data[i - j];
            }
            ma[i] = sum / window;
        }
        return ma;
    }

    std::vector<double> calculateStdDev(
        const std::vector<double>& data,
        const std::vector<double>& ma,
        int window) {
        
        std::vector<double> std_dev(data.size(), 0.0);
        for (size_t i = window - 1; i < data.size(); ++i) {
            double sum_sq = 0;
            for (int j = 0; j < window; ++j) {
                double diff = data[i - j] - ma[i];
                sum_sq += diff * diff;
            }
            std_dev[i] = std::sqrt(sum_sq / window);
        }
        return std_dev;
    }

    double calculateVolScalar(double current_vol, const std::unordered_map<std::string, double>& vol_params) {
        double target_vol = vol_params.at("target_vol");
        if (current_vol > 0) {
            return target_vol / current_vol;
        }
        return 1.0;
    }
}; 