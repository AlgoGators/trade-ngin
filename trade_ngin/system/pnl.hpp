#pragma once
#include "dataframe.hpp"
#include <vector>
#include <cmath>

class PnL {
public:
    PnL(const DataFrame& positions, const DataFrame& prices, double capital, const DataFrame& multipliers)
        : positions_(positions), prices_(prices), initial_capital_(capital), multipliers_(multipliers) {
        calculate();
    }

    // Calculate daily PnL
    void calculate() {
        // Calculate position value at each point
        auto position_values = multiply_dataframes(positions_, prices_);
        position_values = position_values.mul_row(multipliers_);

        // Calculate daily changes
        daily_pnl_.resize(position_values.rows() - 1);
        cumulative_pnl_.resize(position_values.rows() - 1);
        
        double running_pnl = 0.0;
        for (size_t i = 1; i < position_values.rows(); ++i) {
            daily_pnl_[i-1] = calculate_daily_pnl(position_values, i);
            running_pnl += daily_pnl_[i-1];
            cumulative_pnl_[i-1] = running_pnl;
        }
    }

    // Return metrics
    double cumulativeProfit() const {
        return cumulative_pnl_.empty() ? 0.0 : cumulative_pnl_.back();
    }

    double sharpeRatio() const {
        if (daily_pnl_.empty()) return 0.0;
        
        // Calculate mean daily return
        double mean = 0.0;
        for (double pnl : daily_pnl_) {
            mean += pnl;
        }
        mean /= daily_pnl_.size();

        // Calculate standard deviation
        double variance = 0.0;
        for (double pnl : daily_pnl_) {
            double diff = pnl - mean;
            variance += diff * diff;
        }
        variance /= (daily_pnl_.size() - 1);
        
        double daily_std = std::sqrt(variance);
        if (daily_std == 0.0) return 0.0;

        // Annualize Sharpe Ratio (assuming 252 trading days)
        return (mean / daily_std) * std::sqrt(252.0);
    }

    // Getters
    const std::vector<double>& getDailyPnL() const { return daily_pnl_; }
    const std::vector<double>& getCumulativePnL() const { return cumulative_pnl_; }
    double getInitialCapital() const { return initial_capital_; }

private:
    double calculate_daily_pnl(const DataFrame& position_values, size_t i) const {
        // Calculate P&L from position changes and price changes
        double prev_value = position_values.get_value(i-1);
        double curr_value = position_values.get_value(i);
        return curr_value - prev_value;
    }

    DataFrame positions_;
    DataFrame prices_;
    DataFrame multipliers_;
    double initial_capital_;
    std::vector<double> daily_pnl_;
    std::vector<double> cumulative_pnl_;
}; 