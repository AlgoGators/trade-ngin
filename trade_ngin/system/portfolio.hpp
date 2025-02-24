#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <stdexcept>
#include "market_data.hpp"
#include <memory>
#include <numeric>
#include <iostream>

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
          max_drawdown_limit_(0.25),
          total_return_(0.0),
          annualized_return_(0.0),
          sharpe_ratio_(0.0),
          win_rate_(0.0),
          profit_factor_(0.0) {
        std::cout << "Portfolio initialized with capital: " << initial_capital << std::endl;
    }

    double processSignal(const MarketData& data, double signal) {
        std::cout << "\nProcessing signal for " << data.symbol << ": " << signal << std::endl;
        std::cout << "Current capital: " << current_capital_ << ", Peak capital: " << peak_capital_ << std::endl;
        
        double pnl = 0.0;
        
        // Calculate P&L if we have a position
        auto it = positions_.find(data.symbol);
        if (it != positions_.end()) {
            double prev_position = it->second.size;
            double prev_price = it->second.price;
            pnl = prev_position * (data.close - prev_price);
            std::cout << "Existing position - Size: " << prev_position << ", Entry price: " << prev_price << std::endl;
            std::cout << "P&L for position: " << pnl << std::endl;
            
            // Update returns for Sharpe ratio
            daily_returns_.push_back(pnl / current_capital_);
            if (daily_returns_.size() > 1) {
                double mean = std::accumulate(daily_returns_.begin(), daily_returns_.end(), 0.0) / daily_returns_.size();
                double variance = 0.0;
                for (double ret : daily_returns_) {
                    variance += (ret - mean) * (ret - mean);
                }
                variance /= daily_returns_.size();
                sharpe_ratio_ = mean / std::sqrt(variance);
            }
            
            // Update profit/loss tracking
            if (pnl > 0) {
                total_profit_ += pnl;
            } else {
                total_loss_ -= pnl;
            }
        }

        // Check drawdown limit before calculating potential P&L
        if (max_drawdown_limit_ > 0.0 && max_drawdown_ >= max_drawdown_limit_) {
            std::cout << "Drawdown limit exceeded: " << max_drawdown_ << " > " << max_drawdown_limit_ << std::endl;
            throw std::runtime_error("Max drawdown limit exceeded");
        }

        // Calculate target position size based on signal
        double target_size = signal;
        
        // Check position limits
        auto limit_it = position_limits_.find(data.symbol);
        if (limit_it != position_limits_.end() && std::abs(target_size) > limit_it->second) {
            throw std::runtime_error("Position limit exceeded for " + data.symbol + 
                                   ": " + std::to_string(std::abs(target_size)) + 
                                   " > " + std::to_string(limit_it->second));
        }

        // Update position and record trade
        double prev_size = (it != positions_.end()) ? it->second.size : 0.0;
        if (std::abs(target_size - prev_size) > 0.01) {
            // Record trade
            bool is_entry = (prev_size == 0.0);
            recordTrade(data.symbol, target_size - prev_size, data.close, is_entry);
            
            // Update position
            if (std::abs(target_size) < 0.01) {
                positions_.erase(data.symbol);
            } else {
                positions_[data.symbol] = {target_size, data.close};
            }
            
            // Update capital and metrics
            current_capital_ += pnl;
            if (current_capital_ > peak_capital_) {
                peak_capital_ = current_capital_;
            } else {
                double drawdown = (peak_capital_ - current_capital_) / peak_capital_;
                if (drawdown > max_drawdown_) {
                    max_drawdown_ = drawdown;
                }
            }
            
            // Update total return and win rate
            total_return_ = (current_capital_ - initial_capital_) / initial_capital_;
            win_rate_ = total_trades_ > 0 ? static_cast<double>(winning_trades_) / total_trades_ : 0.0;
            
            // Update profit factor
            profit_factor_ = total_loss_ > 0.01 ? total_profit_ / total_loss_ : 
                           total_profit_ > 0.01 ? 1.0 : 0.0;
            
            // Update annualized return (assuming 252 trading days per year)
            if (daily_returns_.size() > 0) {
                double mean_return = std::accumulate(daily_returns_.begin(), daily_returns_.end(), 0.0) / daily_returns_.size();
                annualized_return_ = mean_return * 252.0;
            }
        }

        // Update current position
        current_position_ = 0.0;
        for (const auto& pos : positions_) {
            current_position_ += pos.second.size;
        }

        return pnl;
    }

    void updatePosition(const std::string& symbol, double size, double price) {
        // Check position limits first
        if (position_limits_.count(symbol) > 0 && std::abs(size) > position_limits_[symbol]) {
            throw std::runtime_error("Position size exceeds limit for " + symbol + 
                                   ": " + std::to_string(std::abs(size)) + " > " + 
                                   std::to_string(position_limits_[symbol]));
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
        bool is_new_position = positions_.count(symbol) == 0 || positions_[symbol].size == 0;
        bool is_closing_position = std::abs(size) < 0.01;
        
        if (is_new_position || is_closing_position) {
            total_trades_++;
            if (potential_pnl > 0) {
                winning_trades_++;
            }
        }
        
        // Update position tracking
        positions_[symbol] = Position(size, price);
        if (size > 0) {
            long_positions_[symbol] = Position(size, price);
            short_positions_.erase(symbol);
        } else if (size < 0) {
            short_positions_[symbol] = Position(size, price);
            long_positions_.erase(symbol);
        } else {
            long_positions_.erase(symbol);
            short_positions_.erase(symbol);
        }
        
        // Update capital and metrics
        current_capital_ = potential_capital;
        peak_capital_ = std::max(peak_capital_, current_capital_);
        max_drawdown_ = std::max(max_drawdown_, potential_drawdown);
        
        // Update profit/loss tracking
        if (potential_pnl > 0) {
            total_profit_ += potential_pnl;
        } else if (potential_pnl < 0) {
            total_loss_ += -potential_pnl;
        }
        
        // Update metrics
        win_rate_ = total_trades_ > 0 ? static_cast<double>(winning_trades_) / total_trades_ : 0.0;
        profit_factor_ = total_loss_ > 0 ? total_profit_ / total_loss_ : total_profit_ > 0 ? 1.0 : 0.0;
    }

    void recordTrade(const std::string& symbol, double size, double price, bool is_entry) {
        std::cout << "\nRecording trade for " << symbol << std::endl;
        std::cout << "Size: " << size << ", Price: " << price << ", Is entry: " << is_entry << std::endl;
        
        double pnl = 0.0;
        if (positions_.find(symbol) != positions_.end()) {
            double prev_size = positions_[symbol].size;
            double prev_price = positions_[symbol].price;
            pnl = prev_size * (price - prev_price);
            std::cout << "Previous position - Size: " << prev_size << ", Price: " << prev_price << std::endl;
            std::cout << "P&L for trade: " << pnl << std::endl;
        }

        // Update position
        if (std::abs(size) < 0.01) {
            positions_.erase(symbol);
            std::cout << "Position closed for " << symbol << std::endl;
        } else {
            positions_[symbol] = {size, price};
            std::cout << "Position updated - Size: " << size << ", Price: " << price << std::endl;
        }

        // Update trade statistics
        total_trades_++;
        if (pnl > 0) {
            winning_trades_++;
            total_profit_ += pnl;
        } else if (pnl < 0) {
            total_loss_ -= pnl;  // Convert negative PnL to positive loss
        }

        std::cout << "Trade stats updated - Total trades: " << total_trades_ 
                  << ", Winning trades: " << winning_trades_ 
                  << ", Total profit: " << total_profit_ 
                  << ", Total loss: " << total_loss_ << std::endl;

        // Update metrics
        if (total_trades_ > 0) {
            win_rate_ = static_cast<double>(winning_trades_) / total_trades_;
            std::cout << "Win rate updated: " << win_rate_ << std::endl;
        }
        
        if (total_loss_ > 0) {
            profit_factor_ = total_profit_ / total_loss_;
            std::cout << "Profit factor updated: " << profit_factor_ << std::endl;
        } else if (total_profit_ > 0) {
            profit_factor_ = 2.0;  // If no losses but some profits, set to 2.0
            std::cout << "No losses, profit factor set to 2.0" << std::endl;
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
    std::vector<double> daily_returns_;
}; 