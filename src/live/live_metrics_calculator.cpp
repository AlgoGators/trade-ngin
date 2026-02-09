// src/live/live_metrics_calculator.cpp
// Implementation of pure calculation methods for live trading metrics

#include "trade_ngin/live/live_metrics_calculator.hpp"
#include <numeric>
#include <cmath>
#include <algorithm>

namespace trade_ngin {

// ========== Return Calculations ==========

double LiveMetricsCalculator::calculate_daily_return(
    double daily_pnl,
    double previous_portfolio_value) const {

    if (previous_portfolio_value <= 0.0) {
        // Previous portfolio value is zero or negative, returning 0% daily return
        return 0.0;
    }

    return (daily_pnl / previous_portfolio_value) * 100.0;
}

double LiveMetricsCalculator::calculate_total_return(
    double current_portfolio_value,
    double initial_capital) const {

    if (initial_capital <= 0.0) {
        // LOG_DEBUG << "Initial capital is zero or negative, returning 0% total return";
        return 0.0;
    }

    return ((current_portfolio_value - initial_capital) / initial_capital) * 100.0;
}

double LiveMetricsCalculator::calculate_annualized_return(
    double total_return_decimal,
    int trading_days) const {

    if (trading_days <= 0) {
        // LOG_DEBUG << "Trading days is zero or negative, returning 0% annualized return";
        return 0.0;
    }

    if (trading_days == 1) {
        // Cannot annualize with only 1 day
        // LOG_DEBUG << "Only 1 trading day, returning total return as annualized";
        return total_return_decimal * 100.0;
    }

    // Geometric annualization formula
    // annualized_return = ((1 + total_return)^(252/days) - 1) * 100
    const double trading_days_per_year = 252.0;
    double years = trading_days / trading_days_per_year;

    if (years <= 0.0) {
        return 0.0;
    }

    double annualized = std::pow(1.0 + total_return_decimal, 1.0 / years) - 1.0;
    return annualized * 100.0;
}

// ========== Leverage and Margin Calculations ==========

double LiveMetricsCalculator::calculate_gross_leverage(
    double gross_notional,
    double portfolio_value) const {

    if (portfolio_value <= 0.0) {
        // LOG_DEBUG << "Portfolio value is zero or negative, returning 0 leverage";
        return 0.0;
    }

    return gross_notional / portfolio_value;
}

double LiveMetricsCalculator::calculate_equity_to_margin_ratio(
    double portfolio_value,
    double margin_posted) const {

    if (margin_posted <= 0.0) {
        // LOG_DEBUG << "Margin posted is zero or negative, returning 0 ratio";
        return 0.0;
    }

    return portfolio_value / margin_posted;
}

double LiveMetricsCalculator::calculate_margin_cushion(
    double equity_to_margin_ratio) const {

    // Margin cushion shows how much equity exceeds margin requirement
    // If ratio is 2.0, cushion is 100% (twice the requirement)
    // If ratio is 1.5, cushion is 50% (50% more than requirement)
    return (equity_to_margin_ratio - 1.0) * 100.0;
}

double LiveMetricsCalculator::calculate_cash_available(
    double portfolio_value,
    double margin_posted) const {

    return portfolio_value - margin_posted;
}

// ========== PnL Calculations ==========

double LiveMetricsCalculator::calculate_position_pnl(
    double quantity,
    double entry_price,
    double current_price,
    double point_value) const {

    // PnL = quantity * (current_price - entry_price) * point_value
    // For long positions: positive quantity, profit when current > entry
    // For short positions: negative quantity, profit when current < entry
    return quantity * (current_price - entry_price) * point_value;
}

double LiveMetricsCalculator::calculate_total_position_pnl(
    const std::vector<PositionPnL>& positions) const {

    double total_pnl = 0.0;
    for (const auto& pos : positions) {
        total_pnl += pos.pnl;
    }
    return total_pnl;
}

double LiveMetricsCalculator::calculate_net_pnl(
    double gross_pnl,
    double commissions) const {

    // Net PnL = Gross PnL - Commissions
    // Commissions are always positive costs
    return gross_pnl - commissions;
}

// ========== Risk Metrics Calculations ==========

double LiveMetricsCalculator::calculate_sharpe_ratio(
    const std::vector<double>& returns,
    double risk_free_rate) const {

    if (returns.empty() || returns.size() < 2) {
        // LOG_DEBUG << "Insufficient returns data for Sharpe ratio";
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double std_dev = calculate_std_dev(returns, mean_return);

    if (std_dev <= 0.0) {
        // LOG_DEBUG << "Zero standard deviation, cannot calculate Sharpe ratio";
        return 0.0;
    }

    // Convert risk-free rate from annual to daily
    double daily_rf_rate = risk_free_rate / 252.0;

    // Sharpe ratio = (mean_return - risk_free_rate) / std_dev
    // Annualize by multiplying by sqrt(252)
    double sharpe = (mean_return - daily_rf_rate) / std_dev;
    return sharpe * std::sqrt(252.0);
}

double LiveMetricsCalculator::calculate_sortino_ratio(
    const std::vector<double>& returns,
    double minimum_acceptable_return) const {

    if (returns.empty() || returns.size() < 2) {
        // LOG_DEBUG << "Insufficient returns data for Sortino ratio";
        return 0.0;
    }

    double mean_return = calculate_mean(returns);
    double downside_dev = calculate_downside_deviation(returns, minimum_acceptable_return);

    if (downside_dev <= 0.0) {
        // LOG_DEBUG << "Zero downside deviation, cannot calculate Sortino ratio";
        return 0.0;
    }

    // Convert MAR from annual to daily
    double daily_mar = minimum_acceptable_return / 252.0;

    // Sortino ratio = (mean_return - MAR) / downside_deviation
    // Annualize by multiplying by sqrt(252)
    double sortino = (mean_return - daily_mar) / downside_dev;
    return sortino * std::sqrt(252.0);
}

double LiveMetricsCalculator::calculate_max_drawdown(
    const std::vector<double>& portfolio_values) const {

    if (portfolio_values.empty()) {
        return 0.0;
    }

    double max_dd = 0.0;
    double peak = portfolio_values[0];

    for (size_t i = 1; i < portfolio_values.size(); ++i) {
        if (portfolio_values[i] > peak) {
            peak = portfolio_values[i];
        }

        if (peak > 0.0) {
            double drawdown = ((peak - portfolio_values[i]) / peak) * 100.0;
            if (drawdown > max_dd) {
                max_dd = drawdown;
            }
        }
    }

    return max_dd;
}

double LiveMetricsCalculator::calculate_volatility(
    const std::vector<double>& returns) const {

    if (returns.empty() || returns.size() < 2) {
        return 0.0;
    }

    double std_dev = calculate_std_dev(returns);

    // Annualize the volatility
    return std_dev * std::sqrt(252.0);
}

double LiveMetricsCalculator::calculate_var_95(
    const std::vector<double>& returns) const {

    if (returns.empty()) {
        return 0.0;
    }

    // Copy and sort returns
    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    // Find the 5th percentile (95% VaR)
    size_t index = static_cast<size_t>(sorted_returns.size() * 0.05);
    if (index >= sorted_returns.size()) {
        index = sorted_returns.size() - 1;
    }

    return sorted_returns[index];
}

double LiveMetricsCalculator::calculate_cvar_95(
    const std::vector<double>& returns) const {

    if (returns.empty()) {
        return 0.0;
    }

    // Copy and sort returns
    std::vector<double> sorted_returns = returns;
    std::sort(sorted_returns.begin(), sorted_returns.end());

    // Find the 5th percentile cutoff
    size_t cutoff_index = static_cast<size_t>(sorted_returns.size() * 0.05);
    if (cutoff_index == 0) {
        cutoff_index = 1;
    }

    // Calculate average of all returns below the cutoff
    double sum = 0.0;
    for (size_t i = 0; i < cutoff_index; ++i) {
        sum += sorted_returns[i];
    }

    return sum / cutoff_index;
}

// ========== Composite Calculations ==========

CalculatedMetrics LiveMetricsCalculator::calculate_all_metrics(
    double daily_pnl,
    double previous_portfolio_value,
    double current_portfolio_value,
    double initial_capital,
    double gross_notional,
    double margin_posted,
    int trading_days,
    double daily_transaction_costs) const {

    CalculatedMetrics metrics;

    // Return metrics
    metrics.daily_return = calculate_daily_return(daily_pnl, previous_portfolio_value);
    metrics.total_return = calculate_total_return(current_portfolio_value, initial_capital);

    // Calculate total return as decimal for annualization
    double total_return_decimal = 0.0;
    if (initial_capital > 0.0) {
        total_return_decimal = (current_portfolio_value - initial_capital) / initial_capital;
    }
    metrics.annualized_return = calculate_annualized_return(total_return_decimal, trading_days);

    // Portfolio metrics
    metrics.gross_leverage = calculate_gross_leverage(gross_notional, current_portfolio_value);
    metrics.equity_to_margin_ratio = calculate_equity_to_margin_ratio(current_portfolio_value, margin_posted);
    metrics.margin_cushion = calculate_margin_cushion(metrics.equity_to_margin_ratio);
    metrics.cash_available = calculate_cash_available(current_portfolio_value, margin_posted);

    // PnL metrics
    metrics.daily_pnl = daily_pnl;
    metrics.total_pnl = current_portfolio_value - initial_capital;

    // Trading days
    metrics.trading_days = trading_days;

    // LOG_DEBUG << "Calculated metrics: "
    //           << "daily_return=" << metrics.daily_return << "%, "
    //           << "total_return=" << metrics.total_return << "%, "
    //           << "annualized_return=" << metrics.annualized_return << "%, "
    //           << "leverage=" << metrics.gross_leverage << "x";

    return metrics;
}

CalculatedMetrics LiveMetricsCalculator::calculate_finalization_metrics(
    double realized_pnl,
    double day_before_portfolio,
    double current_portfolio,
    double initial_capital,
    double gross_notional,
    double margin_posted,
    int trading_days,
    double commissions) const {

    CalculatedMetrics metrics;

    // For finalization, daily PnL is the realized PnL
    metrics.daily_pnl = realized_pnl;
    metrics.realized_pnl = realized_pnl;

    // Return metrics
    metrics.daily_return = calculate_daily_return(realized_pnl, day_before_portfolio);
    metrics.total_return = calculate_total_return(current_portfolio, initial_capital);

    // Calculate total return as decimal for annualization
    double total_return_decimal = 0.0;
    if (initial_capital > 0.0) {
        total_return_decimal = (current_portfolio - initial_capital) / initial_capital;
    }
    metrics.annualized_return = calculate_annualized_return(total_return_decimal, trading_days);

    // Portfolio metrics
    metrics.gross_leverage = calculate_gross_leverage(gross_notional, current_portfolio);
    metrics.equity_to_margin_ratio = calculate_equity_to_margin_ratio(current_portfolio, margin_posted);
    metrics.margin_cushion = calculate_margin_cushion(metrics.equity_to_margin_ratio);
    metrics.cash_available = calculate_cash_available(current_portfolio, margin_posted);

    // Total PnL
    metrics.total_pnl = current_portfolio - initial_capital;

    // Trading days
    metrics.trading_days = trading_days;

    // LOG_DEBUG << "Calculated finalization metrics: "
    //           << "realized_pnl=" << realized_pnl << ", "
    //           << "daily_return=" << metrics.daily_return << "%, "
    //           << "total_return=" << metrics.total_return << "%, "
    //           << "leverage=" << metrics.gross_leverage << "x";

    return metrics;
}

// ========== Utility Methods ==========

double LiveMetricsCalculator::calculate_win_rate(
    int winning_trades,
    int losing_trades) const {

    int total_trades = winning_trades + losing_trades;
    if (total_trades <= 0) {
        return 0.0;
    }

    return (static_cast<double>(winning_trades) / total_trades) * 100.0;
}

double LiveMetricsCalculator::calculate_profit_factor(
    double gross_wins,
    double gross_losses) const {

    if (gross_losses <= 0.0) {
        // If no losses, profit factor is infinite (we'll return a large number)
        return gross_wins > 0.0 ? 999.99 : 0.0;
    }

    return gross_wins / gross_losses;
}

// ========== Private Helper Methods ==========

double LiveMetricsCalculator::calculate_mean(const std::vector<double>& values) const {
    if (values.empty()) {
        return 0.0;
    }

    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    return sum / values.size();
}

double LiveMetricsCalculator::calculate_std_dev(
    const std::vector<double>& values,
    double mean) const {

    if (values.empty() || values.size() < 2) {
        return 0.0;
    }

    // If mean not provided, calculate it
    if (mean == 0.0) {
        mean = calculate_mean(values);
    }

    double sum_squared_diff = 0.0;
    for (const auto& val : values) {
        double diff = val - mean;
        sum_squared_diff += diff * diff;
    }

    // Use sample standard deviation (divide by n-1)
    double variance = sum_squared_diff / (values.size() - 1);
    return std::sqrt(variance);
}

double LiveMetricsCalculator::calculate_downside_deviation(
    const std::vector<double>& returns,
    double target) const {

    if (returns.empty() || returns.size() < 2) {
        return 0.0;
    }

    // Calculate sum of squared negative deviations from target
    double sum_squared_negative_diff = 0.0;
    int count = 0;

    for (const auto& ret : returns) {
        if (ret < target) {
            double diff = ret - target;
            sum_squared_negative_diff += diff * diff;
            count++;
        }
    }

    if (count < 2) {
        // Not enough downside observations
        return 0.0;
    }

    // Use sample downside deviation
    double variance = sum_squared_negative_diff / (count - 1);
    return std::sqrt(variance);
}

} // namespace trade_ngin