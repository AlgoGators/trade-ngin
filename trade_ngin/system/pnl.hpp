#pragma once
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>

class PnL {
public:
    struct PerformanceMetrics {
        double total_return;
        double annualized_return;
        double sharpe_ratio;
        double sortino_ratio;
        double max_drawdown;
        double win_rate;
        double profit_factor;
        double avg_win;
        double avg_loss;
        double calmar_ratio;
    };

    PnL(const DataFrame& positions, const DataFrame& prices, 
        double capital, const DataFrame& multipliers)
        : initial_capital_(capital), positions_(positions), 
          prices_(prices), multipliers_(multipliers) {
        calculate();
    }

    void calculate() {
        profits_.clear();
        daily_returns_.clear();
        
        double running_capital = initial_capital_;
        
        for (size_t i = 1; i < prices_.rows(); ++i) {
            auto pos = positions_.get_row(i-1);
            auto price_diff = prices_.get_row(i) - prices_.get_row(i-1);
            auto mult = multipliers_.get_row(i);
            
            double pnl = calculate_daily_pnl(pos, price_diff, mult);
            profits_.push_back(pnl);
            
            double daily_return = pnl / running_capital;
            daily_returns_.push_back(daily_return);
            
            running_capital += pnl;
            updateDrawdown(running_capital);
        }
    }

    PerformanceMetrics getMetrics() const {
        PerformanceMetrics metrics;
        metrics.total_return = calculateTotalReturn();
        metrics.annualized_return = calculateAnnualizedReturn();
        metrics.sharpe_ratio = calculateSharpeRatio();
        metrics.sortino_ratio = calculateSortinoRatio();
        metrics.max_drawdown = max_drawdown_;
        metrics.win_rate = calculateWinRate();
        metrics.profit_factor = calculateProfitFactor();
        metrics.calmar_ratio = metrics.annualized_return / metrics.max_drawdown;
        return metrics;
    }

private:
    double initial_capital_;
    DataFrame positions_;
    DataFrame prices_;
    DataFrame multipliers_;
    std::vector<double> profits_;
    std::vector<double> daily_returns_;
    double max_drawdown_ = 0.0;
    double peak_capital_ = 0.0;

    // Implementation of calculation methods...
    double calculateTotalReturn() const;
    double calculateAnnualizedReturn() const;
    double calculateSharpeRatio() const;
    double calculateSortinoRatio() const;
    double calculateWinRate() const;
    double calculateProfitFactor() const;
    void updateDrawdown(double current_capital);
}; 