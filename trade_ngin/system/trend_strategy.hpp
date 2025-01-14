#pragma once
#include "strategy.hpp"
#include "signals.hpp"
#include "../data/dataframe.hpp"
#include <memory>
#include <unordered_map>

class TrendStrategy : public Strategy {
public:
    TrendStrategy(double capital, const Instrument& instrument, const StrategyConfig& config)
        : Strategy("TrendStrategy", {capital, config.max_leverage, config.pos_limit, config.risk_limit, {}})
        , instrument_(instrument) {}

    void configure(const std::unordered_map<std::string, std::string>& params) {
        short_span_ = std::stoi(params.at("short_span"));
        long_span_ = std::stoi(params.at("long_span"));
        vol_window_ = std::stoi(params.at("vol_window"));
        regime_fast_window_ = std::stoi(params.at("regime_fast_window"));
        regime_slow_window_ = std::stoi(params.at("regime_slow_window"));
    }

    DataFrame positions() override {
        return current_positions_;
    }

    void update(const DataFrame& market_data) override {
        if (market_data.empty()) return;

        // Calculate trend signals
        auto prices = market_data.get_column("close");
        auto returns = calculateReturns(prices);
        auto vol = calculateVolatility(returns, vol_window_);
        
        // Generate trend signals
        auto short_ma = calculateMA(prices, short_span_);
        auto long_ma = calculateMA(prices, long_span_);
        auto trend_signal = generateTrendSignal(short_ma, long_ma);

        // Calculate volatility regime
        auto fast_vol = calculateVolatility(returns, regime_fast_window_);
        auto slow_vol = calculateVolatility(returns, regime_slow_window_);
        auto vol_regime = calculateVolRegime(fast_vol, slow_vol);

        // Combine signals and apply position sizing
        std::vector<double> positions = applyPositionSizing(trend_signal, vol_regime, vol);
        
        // Update positions DataFrame
        std::unordered_map<std::string, std::vector<double>> pos_map;
        pos_map["position"] = positions;
        current_positions_ = DataFrame(pos_map);
    }

private:
    Instrument instrument_;
    int short_span_;
    int long_span_;
    int vol_window_;
    int regime_fast_window_;
    int regime_slow_window_;

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

    std::vector<double> generateTrendSignal(const std::vector<double>& short_ma, 
                                          const std::vector<double>& long_ma) {
        std::vector<double> signal(short_ma.size(), 0.0);
        for (size_t i = 0; i < signal.size(); ++i) {
            if (short_ma[i] > long_ma[i]) {
                signal[i] = 1.0;
            } else if (short_ma[i] < long_ma[i]) {
                signal[i] = -1.0;
            }
        }
        return signal;
    }

    std::vector<double> calculateVolRegime(const std::vector<double>& fast_vol,
                                         const std::vector<double>& slow_vol) {
        std::vector<double> regime(fast_vol.size(), 1.0);
        for (size_t i = 0; i < regime.size(); ++i) {
            if (fast_vol[i] > slow_vol[i] * 1.2) { // High vol regime
                regime[i] = 0.5;
            } else if (fast_vol[i] < slow_vol[i] * 0.8) { // Low vol regime
                regime[i] = 1.5;
            }
        }
        return regime;
    }

    std::vector<double> applyPositionSizing(const std::vector<double>& trend_signal,
                                          const std::vector<double>& vol_regime,
                                          const std::vector<double>& volatility) {
        std::vector<double> positions(trend_signal.size(), 0.0);
        for (size_t i = 0; i < positions.size(); ++i) {
            double vol_scalar = config_.risk_limit / (volatility[i] + 1e-10);
            positions[i] = trend_signal[i] * vol_regime[i] * vol_scalar;
            
            // Apply position limits
            positions[i] = std::max(std::min(positions[i], config_.position_limit), 
                                  -config_.position_limit);
        }
        return positions;
    }
}; 