#pragma once
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>
#include <cmath>
#include <algorithm>
#include <iostream>

struct MarketData {
    std::string timestamp;
    std::string symbol;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

class TrendStrategy {
public:
    TrendStrategy() = default;

    void configureSignals(
        const std::unordered_map<std::string, double>& ma_params,
        const std::unordered_map<std::string, double>& vol_params,
        const std::unordered_map<std::string, double>& regime_params,
        const std::unordered_map<std::string, double>& momentum_params,
        const std::unordered_map<std::string, double>& weight_params
    ) {
        ma_params_      = ma_params;
        vol_params_     = vol_params;
        regime_params_  = regime_params;
        momentum_params_= momentum_params;
        weight_params_  = weight_params;
    }

    // Main signal generator
    std::vector<double> generateSignals(const std::vector<MarketData>& data) {
        if (data.empty()) return {};

        // Collect closing prices
        std::vector<double> prices;
        prices.reserve(data.size());
        for (const auto& bar : data) {
            prices.push_back(bar.close);
        }

        // Calculate daily log returns
        auto returns = calculateReturns(prices);

        // Calculate rolling volatility
        auto volatility = calculateVolatility(returns, static_cast<int>(vol_params_["window"]));

        // Short-term EMAs (70% weight)
        // windows: short_window_1..short_window_6
        std::vector<std::vector<double>> short_emas;
        for (int i = 1; i <= 6; ++i) {
            std::string key = "short_window_" + std::to_string(i);
            auto ema = calculateEMA(prices, static_cast<int>(ma_params_[key]));
            short_emas.push_back(ema);
        }

        // Long-term EMAs (30% weight)
        // windows: long_window_1..long_window_3
        std::vector<std::vector<double>> long_emas;
        for (int i = 1; i <= 3; ++i) {
            std::string key = "long_window_" + std::to_string(i);
            auto ema = calculateEMA(prices, static_cast<int>(ma_params_[key]));
            long_emas.push_back(ema);
        }

        // The largest window sets the earliest day we can produce a signal
        int max_window = static_cast<int>(ma_params_["long_window_3"]);
        std::vector<double> signals(data.size(), 0.0);

        for (size_t i = max_window; i < data.size(); ++i) {
            // short-term signal contribution
            double short_term_signal = 0.0;
            for (auto& ema_vec : short_emas) {
                double trend_val = std::log(prices[i] / ema_vec[i]);
                short_term_signal += trend_val * weight_params_["short_weight"];
            }
            
            // long-term signal contribution
            double long_term_signal = 0.0;
            for (auto& ema_vec : long_emas) {
                double trend_val = std::log(prices[i] / ema_vec[i]);
                long_term_signal += trend_val * weight_params_["long_weight"];
            }
            
            // combine
            double combined_signal = short_term_signal + long_term_signal;

            // regime filter
            double vol_today = volatility[i];
            double regime_strength = std::abs(combined_signal) / (vol_today * std::sqrt(252.0));
            double threshold = regime_params_["threshold"];
            if (regime_strength < threshold) {
                combined_signal *= 0.5; // cut signal in half if regime not strong
            }

            // volatility scaling
            double annual_vol = vol_today * std::sqrt(252.0);
            double target_vol = vol_params_["target_vol"];
            double vol_scale  = target_vol / (annual_vol + 1e-10);

            // high-vol regime
            if (annual_vol > vol_params_["high_vol_threshold"]) {
                vol_scale *= 0.5; 
            }
            else if (annual_vol < vol_params_["low_vol_threshold"]) {
                vol_scale = std::min(2.0, vol_scale);
            }

            double scaled_signal = combined_signal * vol_scale;

            // momentum overlay: if sign of momentum mismatches sign of signal, reduce
            double mom_lookback = momentum_params_["lookback"];
            double momentum_val  = calculateMomentum(returns, i, (int)mom_lookback);
            if ((momentum_val * scaled_signal) < 0) {
                scaled_signal *= 0.5;
            }

            // enforce a minimum position if there is any signal
            if (std::abs(scaled_signal) > 0.1) {
                scaled_signal = std::copysign(std::max(0.1, std::abs(scaled_signal)), scaled_signal);
            }

            // clamp to [-1, 1]
            scaled_signal = std::max(-1.0, std::min(1.0, scaled_signal));

            signals[i] = scaled_signal;
        }

        return signals;
    }

private:
    // Calculate EMA
    std::vector<double> calculateEMA(const std::vector<double>& data, int window) {
        std::vector<double> ema(data.size());
        if (data.empty()) return ema;
        double alpha = 2.0 / (window + 1.0);

        ema[0] = data[0];
        for (size_t i = 1; i < data.size(); ++i) {
            ema[i] = alpha * data[i] + (1.0 - alpha) * ema[i - 1];
        }
        return ema;
    }

    // Calculate daily log returns
    std::vector<double> calculateReturns(const std::vector<double>& prices) {
        std::vector<double> rets(prices.size(), 0.0);
        for (size_t i = 1; i < prices.size(); ++i) {
            rets[i] = std::log(prices[i] / prices[i - 1]);
        }
        return rets;
    }

    // Rolling std dev (annualized) of log returns
    std::vector<double> calculateVolatility(const std::vector<double>& returns, int window) {
        std::vector<double> vol(returns.size(), 0.0);
        double sum_sq = 0.0;
        for (size_t i = 0; i < returns.size(); ++i) {
            sum_sq += returns[i] * returns[i];
            if (i >= (size_t)window) {
                sum_sq -= returns[i - window] * returns[i - window];
            }
            int divisor = (i < (size_t)window) ? (int)i + 1 : window;
            vol[i] = std::sqrt(sum_sq / (divisor + 1e-10));
        }
        return vol;
    }

    double calculateMomentum(const std::vector<double>& returns, size_t idx, int lookback) {
        if (idx < (size_t)lookback) return 0.0;
        double sum = 0.0;
        for (int i = 0; i < lookback; ++i) {
            sum += returns[idx - i];
        }
        return sum;
    }

    std::unordered_map<std::string, double> ma_params_;
    std::unordered_map<std::string, double> vol_params_;
    std::unordered_map<std::string, double> regime_params_;
    std::unordered_map<std::string, double> momentum_params_;
    std::unordered_map<std::string, double> weight_params_;
}; 