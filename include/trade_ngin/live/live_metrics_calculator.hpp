// include/trade_ngin/live/live_metrics_calculator.hpp
// Pure calculation component for live trading metrics - no database dependencies

#pragma once

#include <cmath>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>

namespace trade_ngin {

/**
 * @brief Structure containing all daily metrics calculated for live trading
 */
struct CalculatedMetrics {
    // Return metrics (in percentage)
    double daily_return = 0.0;
    double total_return = 0.0;
    double annualized_return = 0.0;

    // Portfolio metrics
    double portfolio_leverage = 0.0;
    double equity_to_margin_ratio = 0.0;
    double margin_cushion = 0.0;
    double cash_available = 0.0;

    // PnL metrics
    double daily_pnl = 0.0;
    double total_pnl = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;

    // Risk metrics (for future expansion)
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;
    double volatility = 0.0;
    double var_95 = 0.0;
    double cvar_95 = 0.0;

    // Additional metrics
    int trading_days = 0;
    double win_rate = 0.0;
    double profit_factor = 0.0;
};

/**
 * @brief Structure for position PnL calculation
 */
struct PositionPnL {
    std::string symbol;
    double quantity = 0.0;
    double entry_price = 0.0;
    double current_price = 0.0;
    double point_value = 0.0;
    double pnl = 0.0;
};

/**
 * @brief LiveMetricsCalculator - Pure calculation methods for live trading metrics
 *
 * This class contains only pure mathematical functions with no side effects.
 * All methods are const and do not depend on any external state or database.
 * Designed to be reusable across different live trading strategies.
 */
class LiveMetricsCalculator {
public:
    LiveMetricsCalculator() = default;
    ~LiveMetricsCalculator() = default;

    // ========== Return Calculations ==========

    /**
     * @brief Calculate daily return percentage
     * @param daily_pnl Daily profit and loss
     * @param previous_portfolio_value Previous day's portfolio value
     * @return Daily return as percentage
     */
    double calculate_daily_return(
        double daily_pnl,
        double previous_portfolio_value) const;

    /**
     * @brief Calculate total return percentage
     * @param current_portfolio_value Current portfolio value
     * @param initial_capital Initial capital
     * @return Total return as percentage
     */
    double calculate_total_return(
        double current_portfolio_value,
        double initial_capital) const;

    /**
     * @brief Calculate annualized return using geometric method
     * @param total_return_decimal Total return as decimal (not percentage)
     * @param trading_days Number of trading days
     * @return Annualized return as percentage
     */
    double calculate_annualized_return(
        double total_return_decimal,
        int trading_days) const;

    // ========== Leverage and Margin Calculations ==========

    /**
     * @brief Calculate portfolio leverage
     * @param gross_notional Gross notional value
     * @param portfolio_value Portfolio value
     * @return Portfolio leverage ratio
     */
    double calculate_portfolio_leverage(
        double gross_notional,
        double portfolio_value) const;

    /**
     * @brief Calculate equity to margin ratio
     * @param portfolio_value Portfolio value
     * @param margin_posted Margin posted
     * @return Equity to margin ratio
     */
    double calculate_equity_to_margin_ratio(
        double portfolio_value,
        double margin_posted) const;

    /**
     * @brief Calculate margin cushion percentage
     * @param equity_to_margin_ratio Equity to margin ratio
     * @return Margin cushion as percentage
     */
    double calculate_margin_cushion(
        double equity_to_margin_ratio) const;

    /**
     * @brief Calculate available cash
     * @param portfolio_value Portfolio value
     * @param margin_posted Margin posted
     * @return Cash available
     */
    double calculate_cash_available(
        double portfolio_value,
        double margin_posted) const;

    // ========== PnL Calculations ==========

    /**
     * @brief Calculate position PnL
     * @param quantity Position quantity
     * @param entry_price Entry price
     * @param current_price Current market price
     * @param point_value Point value multiplier
     * @return Calculated PnL
     */
    double calculate_position_pnl(
        double quantity,
        double entry_price,
        double current_price,
        double point_value) const;

    /**
     * @brief Calculate total PnL from multiple positions
     * @param positions Vector of position PnL structures
     * @return Total PnL across all positions
     */
    double calculate_total_position_pnl(
        const std::vector<PositionPnL>& positions) const;

    /**
     * @brief Calculate net PnL after commissions
     * @param gross_pnl Gross PnL
     * @param commissions Total commissions
     * @return Net PnL
     */
    double calculate_net_pnl(
        double gross_pnl,
        double commissions) const;

    // ========== Risk Metrics Calculations ==========

    /**
     * @brief Calculate Sharpe ratio
     * @param returns Vector of daily returns
     * @param risk_free_rate Risk-free rate (annual)
     * @return Sharpe ratio
     */
    double calculate_sharpe_ratio(
        const std::vector<double>& returns,
        double risk_free_rate = 0.0) const;

    /**
     * @brief Calculate Sortino ratio
     * @param returns Vector of daily returns
     * @param minimum_acceptable_return Minimum acceptable return
     * @return Sortino ratio
     */
    double calculate_sortino_ratio(
        const std::vector<double>& returns,
        double minimum_acceptable_return = 0.0) const;

    /**
     * @brief Calculate maximum drawdown
     * @param portfolio_values Vector of portfolio values over time
     * @return Maximum drawdown as percentage
     */
    double calculate_max_drawdown(
        const std::vector<double>& portfolio_values) const;

    /**
     * @brief Calculate volatility (standard deviation of returns)
     * @param returns Vector of daily returns
     * @return Annualized volatility
     */
    double calculate_volatility(
        const std::vector<double>& returns) const;

    /**
     * @brief Calculate Value at Risk at 95% confidence
     * @param returns Vector of daily returns
     * @return VaR at 95% confidence
     */
    double calculate_var_95(
        const std::vector<double>& returns) const;

    /**
     * @brief Calculate Conditional Value at Risk at 95% confidence
     * @param returns Vector of daily returns
     * @return CVaR at 95% confidence
     */
    double calculate_cvar_95(
        const std::vector<double>& returns) const;

    // ========== Composite Calculations ==========

    /**
     * @brief Calculate all metrics at once
     * @param daily_pnl Daily PnL
     * @param previous_portfolio_value Previous portfolio value
     * @param current_portfolio_value Current portfolio value
     * @param initial_capital Initial capital
     * @param gross_notional Gross notional value
     * @param margin_posted Margin posted
     * @param trading_days Number of trading days
     * @param daily_transaction_costs Daily transaction costs
     * @return Structure containing all calculated metrics
     */
    CalculatedMetrics calculate_all_metrics(
        double daily_pnl,
        double previous_portfolio_value,
        double current_portfolio_value,
        double initial_capital,
        double gross_notional,
        double margin_posted,
        int trading_days,
        double daily_transaction_costs = 0.0) const;

    /**
     * @brief Calculate metrics for finalization (Day T-1)
     * @param realized_pnl Realized PnL for the day
     * @param day_before_portfolio Previous day's portfolio value
     * @param current_portfolio Current portfolio value
     * @param initial_capital Initial capital
     * @param gross_notional Gross notional value
     * @param margin_posted Margin posted
     * @param trading_days Number of trading days
     * @param commissions Commissions
     * @return Structure containing finalized metrics
     */
    CalculatedMetrics calculate_finalization_metrics(
        double realized_pnl,
        double day_before_portfolio,
        double current_portfolio,
        double initial_capital,
        double gross_notional,
        double margin_posted,
        int trading_days,
        double commissions = 0.0) const;

    // ========== Utility Methods ==========

    /**
     * @brief Calculate win rate from trade results
     * @param winning_trades Number of winning trades
     * @param losing_trades Number of losing trades
     * @return Win rate as percentage
     */
    double calculate_win_rate(
        int winning_trades,
        int losing_trades) const;

    /**
     * @brief Calculate profit factor
     * @param gross_wins Total winning amount
     * @param gross_losses Total losing amount (as positive value)
     * @return Profit factor ratio
     */
    double calculate_profit_factor(
        double gross_wins,
        double gross_losses) const;

private:
    // Helper methods

    /**
     * @brief Calculate mean of a vector
     * @param values Vector of values
     * @return Mean value
     */
    double calculate_mean(const std::vector<double>& values) const;

    /**
     * @brief Calculate standard deviation of a vector
     * @param values Vector of values
     * @param mean Pre-calculated mean (optional)
     * @return Standard deviation
     */
    double calculate_std_dev(
        const std::vector<double>& values,
        double mean = 0.0) const;

    /**
     * @brief Calculate downside deviation for Sortino ratio
     * @param returns Vector of returns
     * @param target Target return
     * @return Downside deviation
     */
    double calculate_downside_deviation(
        const std::vector<double>& returns,
        double target) const;
};

} // namespace trade_ngin