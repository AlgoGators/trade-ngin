#pragma once
#include "signals.hpp"
#include <cmath>

namespace signals {

// Moving Average Crossover Signal
class MACrossoverSignal : public Signal {
public:
    MACrossoverSignal() : short_span_(10), long_span_(50) {}
    
    void configure(const std::unordered_map<std::string, double>& params) override {
        if (params.count("short_span")) {
            short_span_ = static_cast<size_t>(params.at("short_span"));
        }
        if (params.count("long_span")) {
            long_span_ = static_cast<size_t>(params.at("long_span"));
        }
    }
    
    std::vector<double> generate(const DataFrame& data) override {
        const auto& close_prices = data.get_column("close");
        if (close_prices.size() < long_span_) {
            return std::vector<double>(close_prices.size(), 0.0);
        }
        
        // Calculate moving averages
        std::vector<double> short_ma = calculateMA(close_prices, short_span_);
        std::vector<double> long_ma = calculateMA(close_prices, long_span_);
        
        // Calculate crossover signals
        std::vector<double> signals;
        signals.reserve(close_prices.size());
        
        for (size_t i = 0; i < close_prices.size(); ++i) {
            if (i < long_span_ - 1) {
                signals.push_back(0.0);
                continue;
            }
            
            // Generate signal between -1 and 1 based on MA difference
            double diff = short_ma[i] - long_ma[i];
            double signal = std::tanh(diff / (close_prices[i] * 0.01)); // Scale by 1% of price
            signals.push_back(signal);
        }
        
        return signals;
    }

private:
    std::vector<double> calculateMA(const std::vector<double>& data, size_t span) {
        std::vector<double> ma(data.size());
        double sum = 0.0;
        
        // Initial window
        for (size_t i = 0; i < span && i < data.size(); ++i) {
            sum += data[i];
            ma[i] = sum / (i + 1);
        }
        
        // Rolling window
        for (size_t i = span; i < data.size(); ++i) {
            sum = sum - data[i - span] + data[i];
            ma[i] = sum / span;
        }
        
        return ma;
    }
    
    size_t short_span_;
    size_t long_span_;
};

// Volatility-Adjusted Position Signal
class VolatilitySignal : public Signal {
public:
    VolatilitySignal() : window_(20) {}
    
    void configure(const std::unordered_map<std::string, double>& params) override {
        if (params.count("window")) {
            window_ = static_cast<size_t>(params.at("window"));
        }
    }
    
    std::vector<double> generate(const DataFrame& data) override {
        const auto& returns = calculateReturns(data.get_column("close"));
        if (returns.empty()) {
            return std::vector<double>();
        }
        
        std::vector<double> vol_signals;
        vol_signals.reserve(returns.size());
        
        // Calculate rolling volatility
        double sum_sq = 0.0;
        size_t count = 0;
        
        for (size_t i = 0; i < returns.size(); ++i) {
            // Update rolling sum of squared returns
            sum_sq += returns[i] * returns[i];
            if (i >= window_) {
                sum_sq -= returns[i - window_] * returns[i - window_];
            } else {
                count++;
            }
            
            // Calculate volatility
            double vol = std::sqrt(sum_sq / std::min(window_, count) * 252.0);
            
            // Generate inverse volatility signal (higher vol = lower signal)
            double signal = 1.0 / (1.0 + vol);
            vol_signals.push_back(signal);
        }
        
        return vol_signals;
    }

private:
    std::vector<double> calculateReturns(const std::vector<double>& prices) {
        if (prices.size() < 2) return std::vector<double>();
        
        std::vector<double> returns;
        returns.reserve(prices.size() - 1);
        
        for (size_t i = 1; i < prices.size(); ++i) {
            returns.push_back(std::log(prices[i] / prices[i-1]));
        }
        
        return returns;
    }
    
    size_t window_;
};

// Volatility Regime Signal based on Robert Carver's approach
class VolRegimeSignal : public Signal {
public:
    VolRegimeSignal() : window_(20), slow_window_(120) {}
    
    void configure(const std::unordered_map<std::string, double>& params) override {
        if (params.count("window")) {
            window_ = static_cast<size_t>(params.at("window"));
        }
        if (params.count("slow_window")) {
            slow_window_ = static_cast<size_t>(params.at("slow_window"));
        }
    }
    
    std::vector<double> generate(const DataFrame& data) override {
        const auto& returns = calculateReturns(data.get_column("close"));
        if (returns.empty()) {
            return std::vector<double>();
        }
        
        // Calculate fast and slow volatility
        std::vector<double> fast_vol = calculateRollingVol(returns, window_);
        std::vector<double> slow_vol = calculateRollingVol(returns, slow_window_);
        
        std::vector<double> regime_signals;
        regime_signals.reserve(returns.size());
        
        for (size_t i = 0; i < returns.size(); ++i) {
            if (i < slow_window_) {
                regime_signals.push_back(0.0);  // Neutral when not enough data
                continue;
            }
            
            // Compare fast vol to slow vol
            double vol_ratio = fast_vol[i] / slow_vol[i];
            
            // Determine regime:
            // -1.0: High vol regime (vol_ratio > 1.1)
            // 0.0:  Normal vol regime (0.9 <= vol_ratio <= 1.1)
            // 1.0:  Low vol regime (vol_ratio < 0.9)
            double signal;
            if (vol_ratio > 1.1) {
                signal = -1.0;  // High volatility regime
            } else if (vol_ratio < 0.9) {
                signal = 1.0;   // Low volatility regime
            } else {
                signal = 0.0;   // Normal volatility regime
            }
            
            regime_signals.push_back(signal);
        }
        
        return regime_signals;
    }

private:
    std::vector<double> calculateReturns(const std::vector<double>& prices) {
        if (prices.size() < 2) return std::vector<double>();
        
        std::vector<double> returns;
        returns.reserve(prices.size() - 1);
        
        for (size_t i = 1; i < prices.size(); ++i) {
            returns.push_back(std::log(prices[i] / prices[i-1]));
        }
        
        return returns;
    }
    
    std::vector<double> calculateRollingVol(const std::vector<double>& returns, size_t window) {
        std::vector<double> vol(returns.size());
        double sum_sq = 0.0;
        size_t count = 0;
        
        for (size_t i = 0; i < returns.size(); ++i) {
            sum_sq += returns[i] * returns[i];
            if (i >= window) {
                sum_sq -= returns[i - window] * returns[i - window];
            } else {
                count++;
            }
            
            vol[i] = std::sqrt(sum_sq / std::min(window, count) * 252.0);
        }
        
        return vol;
    }
    
    size_t window_;      // Fast volatility window
    size_t slow_window_; // Slow volatility window
};

// New: RSI Mean Reversion Signal
class RSIMeanReversionSignal : public Signal {
public:
    RSIMeanReversionSignal() : period_(14), overbought_(70), oversold_(30) {}
    
    void configure(const std::unordered_map<std::string, double>& params) override {
        if (params.count("period")) period_ = static_cast<size_t>(params.at("period"));
        if (params.count("overbought")) overbought_ = params.at("overbought");
        if (params.count("oversold")) oversold_ = params.at("oversold");
    }
    
    std::vector<double> generate(const DataFrame& data) override {
        const auto& close_prices = data.get_column("close");
        std::vector<double> signals(close_prices.size(), 0.0);
        
        if (close_prices.size() < period_ + 1) return signals;
        
        std::vector<double> gains, losses;
        for (size_t i = 1; i < close_prices.size(); ++i) {
            double change = close_prices[i] - close_prices[i-1];
            gains.push_back(std::max(0.0, change));
            losses.push_back(std::max(0.0, -change));
        }
        
        // Calculate RSI
        for (size_t i = period_; i < close_prices.size(); ++i) {
            double avg_gain = 0.0, avg_loss = 0.0;
            
            // Calculate average gain/loss
            for (size_t j = 0; j < period_; ++j) {
                avg_gain += gains[i-period_+j];
                avg_loss += losses[i-period_+j];
            }
            avg_gain /= period_;
            avg_loss /= period_;
            
            double rs = (avg_loss == 0.0) ? 100.0 : avg_gain / avg_loss;
            double rsi = 100.0 - (100.0 / (1.0 + rs));
            
            // Generate mean reversion signal
            if (rsi > overbought_) {
                signals[i] = -1.0 * ((rsi - overbought_) / (100.0 - overbought_));
            } else if (rsi < oversold_) {
                signals[i] = 1.0 * (1.0 - (rsi / oversold_));
            }
        }
        
        return signals;
    }

private:
    size_t period_;
    double overbought_;
    double oversold_;
};

// New: Momentum Signal
class MomentumSignal : public Signal {
public:
    MomentumSignal() : lookback_(20), volatility_window_(60) {}
    
    void configure(const std::unordered_map<std::string, double>& params) override {
        if (params.count("lookback")) lookback_ = static_cast<size_t>(params.at("lookback"));
        if (params.count("volatility_window")) 
            volatility_window_ = static_cast<size_t>(params.at("volatility_window"));
    }
    
    std::vector<double> generate(const DataFrame& data) override {
        const auto& close_prices = data.get_column("close");
        std::vector<double> signals(close_prices.size(), 0.0);
        
        if (close_prices.size() < lookback_ + volatility_window_) return signals;
        
        // Calculate returns
        std::vector<double> returns;
        for (size_t i = 1; i < close_prices.size(); ++i) {
            returns.push_back(std::log(close_prices[i] / close_prices[i-1]));
        }
        
        // Calculate momentum signals
        for (size_t i = lookback_ + volatility_window_; i < close_prices.size(); ++i) {
            // Calculate momentum
            double momentum_return = close_prices[i] / close_prices[i-lookback_] - 1.0;
            
            // Calculate volatility for scaling
            double sum_squared = 0.0;
            for (size_t j = 0; j < volatility_window_; ++j) {
                sum_squared += returns[i-j-1] * returns[i-j-1];
            }
            double volatility = std::sqrt(sum_squared / volatility_window_ * 252.0);
            
            // Scale momentum by volatility
            signals[i] = momentum_return / (volatility + 1e-6);  // Add small constant to avoid division by zero
            
            // Normalize signal to [-1, 1] range using tanh
            signals[i] = std::tanh(signals[i]);
        }
        
        return signals;
    }

private:
    size_t lookback_;
    size_t volatility_window_;
};

// New: Adaptive Signal Combiner with Dynamic Weights
class AdaptiveSignalCombiner : public SignalCombiner {
public:
    std::vector<double> combine(const std::vector<std::vector<double>>& signals,
                              const std::vector<double>& base_weights) override {
        if (signals.empty() || signals[0].empty()) return std::vector<double>();
        
        size_t n = signals[0].size();
        std::vector<double> combined(n, 0.0);
        std::vector<double> adaptive_weights = base_weights;
        
        // Calculate performance metrics for each signal
        std::vector<double> signal_performance(signals.size(), 1.0);
        
        // Simple performance tracking (can be enhanced)
        for (size_t i = 0; i < signals.size(); ++i) {
            double correct_predictions = 0.0;
            for (size_t j = 1; j < n; ++j) {
                if ((signals[i][j-1] > 0 && signals[i][j] > signals[i][j-1]) ||
                    (signals[i][j-1] < 0 && signals[i][j] < signals[i][j-1])) {
                    correct_predictions += 1.0;
                }
            }
            signal_performance[i] = correct_predictions / (n - 1);
        }
        
        // Adjust weights based on performance
        double total_performance = 0.0;
        for (double perf : signal_performance) total_performance += perf;
        
        for (size_t i = 0; i < adaptive_weights.size(); ++i) {
            adaptive_weights[i] *= (signal_performance[i] / total_performance);
        }
        
        // Combine signals using adaptive weights
        for (size_t i = 0; i < n; ++i) {
            double weighted_sum = 0.0;
            double weight_sum = 0.0;
            
            for (size_t j = 0; j < signals.size(); ++j) {
                weighted_sum += signals[j][i] * adaptive_weights[j];
                weight_sum += adaptive_weights[j];
            }
            
            combined[i] = weighted_sum / weight_sum;
        }
        
        return combined;
    }
};

} // namespace signals