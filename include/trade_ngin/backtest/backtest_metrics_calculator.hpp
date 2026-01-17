#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <map>
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

// Forward declaration
namespace backtest {
struct BacktestResults;
}

/**
 * @brief Pure stateless calculation component for backtest metrics
 *
 * This class extracts the metrics calculation logic from BacktestEngine::calculate_metrics()
 * (lines 2110-2516). All methods are const and have no side effects.
 *
 * Key responsibilities:
 * - Return calculations (total, annualized, daily)
 * - Risk-adjusted metrics (Sharpe, Sortino, Calmar)
 * - Drawdown calculations
 * - Trade statistics (win rate, profit factor, etc.)
 * - Per-symbol P&L breakdown
 * - Monthly returns aggregation
 *
 * Design principles:
 * - All methods are const (no state mutation)
 * - No database dependencies
 * - No logging (caller is responsible for logging)
 * - Pure mathematical functions
 */
class BacktestMetricsCalculator {
public:
    BacktestMetricsCalculator() = default;
    ~BacktestMetricsCalculator() = default;

    // ========== Return Calculations ==========

    /**
     * @brief Calculate total return from equity curve
     * @param start_value Starting portfolio value
     * @param end_value Ending portfolio value
     * @return Total return as decimal (0.10 = 10%)
     */
    double calculate_total_return(double start_value, double end_value) const;

    /**
     * @brief Calculate annualized return
     * @param total_return Total return as decimal
     * @param trading_days Number of trading days
     * @return Annualized return as decimal
     */
    double calculate_annualized_return(double total_return, int trading_days) const;

    /**
     * @brief Calculate daily returns from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Vector of daily returns
     */
    std::vector<double> calculate_returns_from_equity(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Risk-Adjusted Return Metrics ==========

    /**
     * @brief Calculate Sharpe ratio
     * @param returns Vector of daily returns
     * @param trading_days Number of trading days (for annualization)
     * @param risk_free_rate Annual risk-free rate (default 0)
     * @return Sharpe ratio
     */
    double calculate_sharpe_ratio(
        const std::vector<double>& returns,
        int trading_days,
        double risk_free_rate = 0.0) const;

    /**
     * @brief Calculate Sortino ratio
     * @param returns Vector of daily returns
     * @param trading_days Number of trading days (for annualization)
     * @param minimum_acceptable_return Minimum acceptable return (default 0)
     * @return Sortino ratio
     */
    double calculate_sortino_ratio(
        const std::vector<double>& returns,
        int trading_days,
        double minimum_acceptable_return = 0.0) const;

    /**
     * @brief Calculate Calmar ratio
     * @param total_return Total return as decimal
     * @param max_drawdown Maximum drawdown as decimal
     * @return Calmar ratio
     */
    double calculate_calmar_ratio(double total_return, double max_drawdown) const;

    // ========== Volatility Metrics ==========

    /**
     * @brief Calculate annualized volatility
     * @param returns Vector of daily returns
     * @return Annualized volatility (using sqrt(252))
     */
    double calculate_volatility(const std::vector<double>& returns) const;

    /**
     * @brief Calculate downside volatility (for Sortino)
     * @param returns Vector of daily returns
     * @param target Target return (typically 0)
     * @return Annualized downside volatility
     */
    double calculate_downside_volatility(
        const std::vector<double>& returns,
        double target = 0.0) const;

    // ========== Drawdown Metrics ==========

    /**
     * @brief Calculate drawdown curve from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Vector of (timestamp, drawdown) pairs where drawdown is as decimal (0.10 = 10%)
     */
    std::vector<std::pair<Timestamp, double>> calculate_drawdowns(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    /**
     * @brief Calculate maximum drawdown from equity curve
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Maximum drawdown as decimal
     */
    double calculate_max_drawdown(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Risk Metrics ==========

    /**
     * @brief Calculate Value at Risk at 95% confidence
     * @param returns Vector of daily returns
     * @return VaR as positive decimal (loss amount)
     */
    double calculate_var_95(const std::vector<double>& returns) const;

    /**
     * @brief Calculate Conditional VaR (Expected Shortfall) at 95%
     * @param returns Vector of daily returns
     * @return CVaR as positive decimal
     */
    double calculate_cvar_95(const std::vector<double>& returns) const;

    /**
     * @brief Calculate all risk metrics at once
     * @param returns Vector of daily returns
     * @param trading_days Number of trading days
     * @return Map of metric name to value
     */
    std::unordered_map<std::string, double> calculate_risk_metrics(
        const std::vector<double>& returns,
        int trading_days) const;

    // ========== Trade Statistics ==========

    /**
     * @brief Trade statistics result structure
     */
    struct TradeStatistics {
        int total_trades = 0;
        int winning_trades = 0;
        double win_rate = 0.0;
        double profit_factor = 0.0;
        double total_profit = 0.0;
        double total_loss = 0.0;
        double avg_win = 0.0;
        double avg_loss = 0.0;
        double max_win = 0.0;
        double max_loss = 0.0;
        double avg_holding_period = 0.0;
        std::vector<ExecutionReport> actual_trades;  // Position-closing trades only
    };

    /**
     * @brief Calculate trade statistics from executions
     * @param executions Vector of execution reports
     * @return TradeStatistics structure
     */
    TradeStatistics calculate_trade_statistics(
        const std::vector<ExecutionReport>& executions) const;

    // ========== Per-Symbol Analysis ==========

    /**
     * @brief Calculate P&L breakdown by symbol
     * @param executions Vector of execution reports
     * @return Map of symbol to realized P&L
     */
    std::map<std::string, double> calculate_symbol_pnl(
        const std::vector<ExecutionReport>& executions) const;

    /**
     * @brief Calculate monthly returns
     * @param equity_curve Vector of (timestamp, portfolio_value) pairs
     * @return Map of "YYYY-MM" to monthly return
     */
    std::unordered_map<std::string, double> calculate_monthly_returns(
        const std::vector<std::pair<Timestamp, double>>& equity_curve) const;

    // ========== Beta and Correlation ==========

    /**
     * @brief Calculate beta and correlation (simplified self-correlation)
     * @param returns Vector of daily returns
     * @return Pair of (beta, correlation)
     */
    std::pair<double, double> calculate_beta_correlation(
        const std::vector<double>& returns) const;

    // ========== Composite Calculation ==========

    /**
     * @brief Calculate all metrics and populate BacktestResults
     *
     * This is the main entry point that computes all metrics at once.
     *
     * @param equity_curve Full equity curve including warmup period
     * @param executions All execution reports
     * @param warmup_days Number of days to exclude from metric calculations
     * @return Populated BacktestResults structure
     */
    backtest::BacktestResults calculate_all_metrics(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        const std::vector<ExecutionReport>& executions,
        int warmup_days = 0) const;

private:
    // ========== Helper Methods ==========

    /**
     * @brief Calculate mean of a vector
     */
    double calculate_mean(const std::vector<double>& values) const;

    /**
     * @brief Calculate standard deviation
     */
    double calculate_std_dev(const std::vector<double>& values, double mean) const;

    /**
     * @brief Filter equity curve to exclude warmup period
     */
    std::vector<std::pair<Timestamp, double>> filter_warmup_period(
        const std::vector<std::pair<Timestamp, double>>& equity_curve,
        int warmup_days) const;
};

} // namespace trade_ngin
