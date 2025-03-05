#include "performance_analytics.hpp"
#include <cmath>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <iomanip>

// Assuming 2% annual risk-free rate
const double RISK_FREE_RATE = 0.02 / 252.0; // Daily rate

double PerformanceAnalytics::calculateSharpe(const std::vector<double>& returns) const {
    if (returns.empty()) return 0.0;
    
    double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double excess_return = mean_return - RISK_FREE_RATE;
    
    double variance = 0.0;
    for (const auto& ret : returns) {
        variance += std::pow(ret - mean_return, 2);
    }
    variance /= (returns.size() - 1);
    
    double volatility = std::sqrt(variance);
    return volatility == 0 ? 0 : excess_return / volatility;
}

double PerformanceAnalytics::calculateSortino(const std::vector<double>& returns) const {
    if (returns.empty()) return 0.0;
    
    double mean_return = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double excess_return = mean_return - RISK_FREE_RATE;
    
    double downside_variance = 0.0;
    int downside_count = 0;
    for (const auto& ret : returns) {
        if (ret < RISK_FREE_RATE) {
            downside_variance += std::pow(ret - RISK_FREE_RATE, 2);
            downside_count++;
        }
    }
    
    if (downside_count == 0) return 0.0;
    downside_variance /= downside_count;
    
    double downside_deviation = std::sqrt(downside_variance);
    return downside_deviation == 0 ? 0 : excess_return / downside_deviation;
}

std::vector<double> PerformanceAnalytics::calculateDrawdowns() const {
    std::vector<double> drawdowns;
    if (equity_curve_.empty()) return drawdowns;
    
    double max_drawdown = 0.0;
    double peak = equity_curve_[0];
    
    for (const auto& value : equity_curve_) {
        if (value > peak) {
            peak = value;
        }
        double drawdown = (peak - value) / peak;
        max_drawdown = std::max(max_drawdown, drawdown);
    }
    
    drawdowns.push_back(max_drawdown);
    return drawdowns;
}

double PerformanceAnalytics::calculateCalmar() const {
    if (daily_returns_.empty() || equity_curve_.empty()) return 0.0;
    
    double mean_return = std::accumulate(daily_returns_.begin(), daily_returns_.end(), 0.0) / daily_returns_.size();
    double annualized_return = mean_return * 252;  // Assuming daily returns
    
    auto drawdowns = calculateDrawdowns();
    double max_drawdown = drawdowns.empty() ? 0.0 : drawdowns[0];
    
    return max_drawdown == 0 ? 0 : annualized_return / max_drawdown;
}

double PerformanceAnalytics::calculateVaR(double confidence_level) const {
    if (daily_returns_.empty()) return 0.0;
    
    std::vector<double> sorted_returns = daily_returns_;
    std::sort(sorted_returns.begin(), sorted_returns.end());
    
    int index = static_cast<int>((1.0 - confidence_level) * sorted_returns.size());
    return sorted_returns[index];
}

PerformanceAnalytics::TradeStats PerformanceAnalytics::getHistoricalStats() const {
    TradeStats stats;
    if (trade_history_.empty()) return stats;
    
    stats.total_trades = trade_history_.size();
    int winning_trades = 0;
    double total_profit = 0.0;
    double total_loss = 0.0;
    
    for (const auto& trade : trade_history_) {
        if (trade.pnl > 0) {
            winning_trades++;
            total_profit += trade.pnl;
        } else {
            total_loss -= trade.pnl;
        }
    }
    
    stats.win_rate = static_cast<double>(winning_trades) / trade_history_.size();
    stats.profit_factor = total_loss == 0 ? 0 : total_profit / total_loss;
    
    // Calculate additional metrics
    stats.sharpe_ratio = calculateSharpe(daily_returns_);
    stats.sortino_ratio = calculateSortino(daily_returns_);
    stats.calmar_ratio = calculateCalmar();
    stats.var_95 = calculateVaR(0.95);
    stats.cvar_95 = calculateVaR(0.99);
    
    // Calculate drawdown metrics
    auto drawdowns = calculateDrawdowns();
    stats.max_drawdown = drawdowns.empty() ? 0.0 : drawdowns[0];
    
    // Portfolio metrics
    stats.portfolio_beta = calculateBeta(daily_returns_, std::vector<double>());
    stats.rolling_var = calculateRollingMetric(daily_returns_, 20, 
        [this](const std::vector<double>& data) { return calculateVaR(0.95); });
    stats.rolling_sharpe = calculateRollingMetric(daily_returns_, 60,
        [this](const std::vector<double>& data) { return calculateSharpe(data); });
    
    return stats;
}

double PerformanceAnalytics::calculateBeta(const std::vector<double>& returns, const std::vector<double>& benchmark) const {
    if (returns.empty() || benchmark.empty() || returns.size() != benchmark.size()) {
        return 0.0;
    }
    
    double mean_returns = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double mean_benchmark = std::accumulate(benchmark.begin(), benchmark.end(), 0.0) / benchmark.size();
    
    double covariance = 0.0;
    double benchmark_variance = 0.0;
    
    for (size_t i = 0; i < returns.size(); ++i) {
        covariance += (returns[i] - mean_returns) * (benchmark[i] - mean_benchmark);
        benchmark_variance += std::pow(benchmark[i] - mean_benchmark, 2);
    }
    
    covariance /= (returns.size() - 1);
    benchmark_variance /= (benchmark.size() - 1);
    
    return benchmark_variance == 0 ? 0 : covariance / benchmark_variance;
}

std::vector<double> PerformanceAnalytics::calculateRollingMetric(
    const std::vector<double>& data,
    int window,
    std::function<double(const std::vector<double>&)> metric_func) const {
    
    std::vector<double> rolling_metrics;
    if (data.size() < static_cast<size_t>(window)) {
        return rolling_metrics;
    }
    
    for (size_t i = window; i <= data.size(); ++i) {
        std::vector<double> window_data(data.begin() + (i - window), data.begin() + i);
        rolling_metrics.push_back(metric_func(window_data));
    }
    
    return rolling_metrics;
}

PerformanceAnalytics::RealTimeMetrics PerformanceAnalytics::getCurrentMetrics() const {
    RealTimeMetrics metrics;
    
    // Calculate current portfolio state
    metrics.current_equity = equity_curve_.empty() ? 0.0 : equity_curve_.back();
    metrics.cash_balance = metrics.current_equity; // This should be updated with actual cash balance
    metrics.margin_used = 0.0; // This should be updated with actual margin usage
    metrics.buying_power = metrics.current_equity - metrics.margin_used;
    
    // Calculate risk exposure
    metrics.current_var = calculateVaR(0.95);
    metrics.current_leverage = metrics.margin_used / metrics.current_equity;
    metrics.net_exposure = 0.0;
    metrics.gross_exposure = 0.0;
    
    // Calculate position metrics
    for (const auto& [symbol, position] : symbol_returns_) {
        RealTimeMetrics::PositionMetrics pos_metrics;
        // This should be updated with actual position data
        pos_metrics.quantity = 0.0;
        pos_metrics.avg_price = 0.0;
        pos_metrics.current_price = 0.0;
        pos_metrics.unrealized_pnl = 0.0;
        pos_metrics.realized_pnl = 0.0;
        pos_metrics.position_var = calculateVaR(0.95);
        pos_metrics.position_beta = calculateBeta(position, std::vector<double>());
        
        metrics.positions[symbol] = pos_metrics;
        
        // Update exposure
        metrics.gross_exposure += std::abs(pos_metrics.quantity * pos_metrics.current_price);
        metrics.net_exposure += pos_metrics.quantity * pos_metrics.current_price;
    }
    
    // Calculate today's activity
    metrics.today.trades_today = 0;
    metrics.today.today_pnl = 0.0;
    metrics.today.today_fees = 0.0;
    metrics.today.today_turnover = 0.0;
    
    // Get today's trades
    auto now = std::chrono::system_clock::now();
    time_t now_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_t);
    int today_year = now_tm->tm_year;
    int today_month = now_tm->tm_mon;
    int today_day = now_tm->tm_mday;
    
    for (const auto& trade : trade_history_) {
        std::tm trade_tm = {};
        std::istringstream ss(trade.entry_time);
        ss >> std::get_time(&trade_tm, "%Y-%m-%d %H:%M:%S");
        
        // Compare year, month, and day
        if (trade_tm.tm_year == today_year &&
            trade_tm.tm_mon == today_month &&
            trade_tm.tm_mday == today_day) {
            metrics.today.trades_today++;
            metrics.today.today_pnl += trade.pnl;
            metrics.today.today_fees += trade.fees;
            metrics.today.today_turnover += std::abs(trade.quantity * trade.entry_price);
        }
    }
    
    return metrics;
}
