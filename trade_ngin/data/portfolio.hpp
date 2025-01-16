#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include "market_data.hpp"

class Portfolio {
public:
    Portfolio(double initial_capital) 
        : initial_capital_(initial_capital)
        , current_capital_(initial_capital)
        , current_position_(0)
        , total_trades_(0)
        , winning_trades_(0)
        , total_profit_(0.0)
        , total_loss_(0.0)
        , max_drawdown_(0.0)
        , peak_capital_(initial_capital)
        , returns_()
        , daily_returns_()
    {}

    void processSignal(const MarketData& data, double signal) {
        // Calculate P&L from previous position
        if (current_position_ != 0) {
            double pnl = current_position_ * (data.close - prev_close_);
            current_capital_ += pnl;
            
            // Update trade statistics
            if (pnl > 0) {
                total_profit_ += pnl;
                winning_trades_++;
            } else if (pnl < 0) {
                total_loss_ -= pnl;  // Convert to positive for calculations
            }
            if (pnl != 0) total_trades_++;
            
            // Update drawdown
            if (current_capital_ > peak_capital_) {
                peak_capital_ = current_capital_;
            } else {
                double drawdown = (peak_capital_ - current_capital_) / peak_capital_;
                max_drawdown_ = std::max(max_drawdown_, drawdown);
            }
            
            // Store daily return
            double daily_return = pnl / current_capital_;
            daily_returns_.push_back(daily_return);
            returns_.push_back(daily_return);
        }
        
        // Update position based on signal
        current_position_ = signal * current_capital_ / data.close;
        prev_close_ = data.close;
    }

    void updateMetrics(const MarketData& data) {
        // Update time-based metrics here if needed
    }

    // Performance metrics
    double getTotalReturn() const {
        return (current_capital_ - initial_capital_) / initial_capital_;
    }

    double getAnnualizedReturn() const {
        if (returns_.empty()) return 0.0;
        double total_return = getTotalReturn();
        double years = returns_.size() / 252.0;  // Assuming 252 trading days per year
        return std::pow(1.0 + total_return, 1.0 / years) - 1.0;
    }

    double getSharpeRatio() const {
        if (daily_returns_.empty()) return 0.0;
        
        // Calculate mean return
        double sum = 0.0;
        for (double ret : daily_returns_) {
            sum += ret;
        }
        double mean = sum / daily_returns_.size();
        
        // Calculate standard deviation
        double sum_sq = 0.0;
        for (double ret : daily_returns_) {
            double diff = ret - mean;
            sum_sq += diff * diff;
        }
        double std_dev = std::sqrt(sum_sq / (daily_returns_.size() - 1));
        
        // Annualize
        return std::sqrt(252.0) * mean / std_dev;
    }

    double getWinRate() const {
        return total_trades_ > 0 ? static_cast<double>(winning_trades_) / total_trades_ : 0.0;
    }

    double getProfitFactor() const {
        return total_loss_ > 0 ? total_profit_ / total_loss_ : total_profit_ > 0 ? 1.0 : 0.0;
    }

    double getMaxDrawdown() const {
        return max_drawdown_;
    }

    // New accessor methods
    double getCurrentCapital() const { return current_capital_; }
    double getCurrentPosition() const { return current_position_; }
    int getTotalTrades() const { return total_trades_; }
    int getWinningTrades() const { return winning_trades_; }

private:
    double initial_capital_;
    double current_capital_;
    double current_position_;
    double prev_close_;
    
    // Trade statistics
    int total_trades_;
    int winning_trades_;
    double total_profit_;
    double total_loss_;
    double max_drawdown_;
    double peak_capital_;
    
    // Performance tracking
    std::vector<double> returns_;
    std::vector<double> daily_returns_;
}; 