#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <stdexcept>
#include "market_data.hpp"
#include <memory>
#include <numeric>

struct Position {
    double size;
    double price;
    Position(double s = 0.0, double p = 0.0) : size(s), price(p) {}
};

struct TradeStats {
    int total_trades;
    int winning_trades;
    double win_rate;
    double avg_profit_per_trade;
    double sharpe_ratio;
};

class Portfolio {
public:
    explicit Portfolio(double initial_capital) 
        : initial_capital_(initial_capital),
          current_capital_(initial_capital),
          current_position_(0.0),
          prev_close_(0.0),
          total_trades_(0),
          winning_trades_(0),
          total_profit_(0.0),
          total_loss_(0.0),
          max_drawdown_(0.0),
          peak_capital_(initial_capital),
          max_drawdown_limit_(0.0),
          total_return_(0.0),
          annualized_return_(0.0),
          sharpe_ratio_(0.0),
          win_rate_(0.0),
          profit_factor_(0.0) {}

    void processSignal(const MarketData& data, double signal) {
        if (prev_close_ > 0.0) {
            double pnl = (data.close - prev_close_) * current_position_;
            
            // Check drawdown limit before applying PnL
            double potential_capital = current_capital_ + pnl;
            double potential_drawdown = (peak_capital_ - potential_capital) / peak_capital_;
            if (max_drawdown_limit_ > 0.0 && potential_drawdown > max_drawdown_limit_) {
                throw std::runtime_error("Trade would exceed maximum drawdown limit of " + 
                                       std::to_string(max_drawdown_limit_ * 100) + "%");
            }
            
            // Update capital and metrics
            current_capital_ = potential_capital;
            peak_capital_ = std::max(peak_capital_, current_capital_);
            max_drawdown_ = std::max(max_drawdown_, potential_drawdown);
            
            // Record trade if position changed
            if (std::abs(signal - current_position_) > 0.01) {
                recordTrade(data.symbol, signal - current_position_, data.close, signal != 0.0);
            }
            
            // Update returns for Sharpe ratio
            double daily_return = pnl / prev_close_;
            returns_.push_back(daily_return);
            
            // Update Sharpe ratio (annualized)
            if (returns_.size() > 1) {
                double mean_return = std::accumulate(returns_.begin(), returns_.end(), 0.0) / returns_.size();
                double sum_squared_diff = 0.0;
                for (double ret : returns_) {
                    sum_squared_diff += (ret - mean_return) * (ret - mean_return);
                }
                double std_dev = std::sqrt(sum_squared_diff / (returns_.size() - 1));
                sharpe_ratio_ = std_dev > 0 ? (mean_return / std_dev) * std::sqrt(252) : 0.0;
            }
        }
        
        // Update position and metrics
        current_position_ = signal;
        prev_close_ = data.close;
        total_return_ = (current_capital_ - initial_capital_) / initial_capital_;
        win_rate_ = total_trades_ > 0 ? static_cast<double>(winning_trades_) / total_trades_ : 0.0;
        profit_factor_ = total_loss_ > 0 ? total_profit_ / total_loss_ : total_profit_ > 0 ? 1.0 : 0.0;
    }

    void updatePosition(const std::string& symbol, double size, double price) {
        // Check position limits
        if (position_limits_.count(symbol) > 0 && std::abs(size) > position_limits_[symbol]) {
            throw std::runtime_error("Position size exceeds limit for " + symbol);
        }

        // Calculate potential P&L
        double potential_pnl = 0.0;
        if (positions_.count(symbol) > 0) {
            potential_pnl = (price - positions_[symbol].price) * positions_[symbol].size;
        }
        
        // Check drawdown limit
        double potential_capital = current_capital_ + potential_pnl;
        double potential_drawdown = (peak_capital_ - potential_capital) / peak_capital_;
        
        if (max_drawdown_limit_ > 0.0 && potential_drawdown > max_drawdown_limit_) {
            throw std::runtime_error("Trade would exceed maximum drawdown limit of " + 
                                   std::to_string(max_drawdown_limit_ * 100) + "%");
        }

        // Update position and metrics
        positions_[symbol] = Position(size, price);
        current_capital_ = potential_capital;
        peak_capital_ = std::max(peak_capital_, current_capital_);
        max_drawdown_ = std::max(max_drawdown_, potential_drawdown);
        
        // Update profit/loss tracking
        if (potential_pnl > 0) {
            total_profit_ += potential_pnl;
        } else {
            total_loss_ += -potential_pnl;
        }
    }

    void recordTrade(const std::string& symbol, double size, double price, bool is_entry) {
        if (is_entry) {
            total_trades_++;
            if (size > 0) {
                long_positions_[symbol] = Position(size, price);
            } else {
                short_positions_[symbol] = Position(size, price);
            }
        } else {
            // Calculate P&L
            double pnl = 0.0;
            if (size > 0 && short_positions_.count(symbol) > 0) {
                pnl = (short_positions_[symbol].price - price) * std::abs(size);
                short_positions_.erase(symbol);
            } else if (size < 0 && long_positions_.count(symbol) > 0) {
                pnl = (price - long_positions_[symbol].price) * std::abs(size);
                long_positions_.erase(symbol);
            }
            
            if (pnl > 0) {
                winning_trades_++;
                total_profit_ += pnl;
            } else {
                total_loss_ += -pnl;
            }
        }
    }

    // Getters
    double getCurrentCapital() const { return current_capital_; }
    double getCurrentPosition() const { return current_position_; }
    double getTotalReturn() const { return total_return_; }
    double getAnnualizedReturn() const {
        if (returns_.empty()) return 0.0;
        double total_return = getTotalReturn();
        double years = returns_.size() / 252.0;  // Assuming 252 trading days per year
        return std::pow(1.0 + total_return, 1.0 / years) - 1.0;
    }
    double getSharpeRatio() const { return sharpe_ratio_; }
    double getWinRate() const { return win_rate_; }
    double getProfitFactor() const { return profit_factor_; }
    double getMaxDrawdown() const { return max_drawdown_; }
    int getTotalTrades() const { return total_trades_; }
    int getWinningTrades() const { return winning_trades_; }

    void setPositionLimit(const std::string& symbol, double limit) {
        position_limits_[symbol] = limit;
    }

    void setMaxDrawdown(double limit) {
        max_drawdown_limit_ = limit;
    }

    TradeStats getTradeStats() const {
        TradeStats stats;
        stats.total_trades = total_trades_;
        stats.winning_trades = winning_trades_;
        stats.win_rate = win_rate_;
        stats.avg_profit_per_trade = total_trades_ > 0 ? (total_profit_ - total_loss_) / total_trades_ : 0.0;
        stats.sharpe_ratio = sharpe_ratio_;
        return stats;
    }

    const std::unordered_map<std::string, Position>& getPositions() const {
        return positions_;
    }

private:
    double initial_capital_;
    double current_capital_;
    double current_position_;
    double prev_close_;
    double peak_capital_;
    
    // Trade statistics
    int total_trades_;
    int winning_trades_;
    double total_profit_;
    double total_loss_;
    double max_drawdown_;
    double max_drawdown_limit_;
    double total_return_;
    double annualized_return_;
    double sharpe_ratio_;
    double win_rate_;
    double profit_factor_;
    
    std::vector<double> returns_;
    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, Position> long_positions_;
    std::unordered_map<std::string, Position> short_positions_;
    std::unordered_map<std::string, double> position_limits_;
}; 