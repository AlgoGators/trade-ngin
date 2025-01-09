#ifndef PNL_H
#define PNL_H

#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>

class PNL {
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

    PNL(double capital, double contract_size) 
        : initial_capital_(capital), contract_size_(contract_size) {}

    void calculate(const std::vector<double>& positions, const std::vector<double>& prices) {
        profits_.clear();
        daily_returns_.clear();
        
        double running_capital = initial_capital_;
        
        for (size_t i = 1; i < prices.size(); ++i) {
            if (std::isnan(positions[i-1]) || std::isnan(prices[i]) || std::isnan(prices[i-1])) {
                profits_.push_back(0.0);
                daily_returns_.push_back(0.0);
                continue;
            }

            double pnl = positions[i-1] * (prices[i] - prices[i-1]) * contract_size_;
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
    double contract_size_;
    std::vector<double> profits_;
    std::vector<double> daily_returns_;
    double max_drawdown_ = 0.0;
    double peak_capital_ = 0.0;

    void updateDrawdown(double current_capital) {
        if (current_capital > peak_capital_) {
            peak_capital_ = current_capital;
        }
        double drawdown = (peak_capital_ - current_capital) / peak_capital_;
        max_drawdown_ = std::max(max_drawdown_, drawdown);
    }

    // Implementation of various calculation methods...
};

#endif
