#pragma once

#include <vector>

namespace trade_ngin {

/**
 * @brief Aggregate structure for since-inception performance metrics.
 *
 * Units:
 * - Returns-related fields (volatility, downside_deviation, avg_win, avg_loss,
 *   best_day, worst_day) are in percentage points, consistent with daily_return.
 * - Ratios (sharpe_ratio, sortino_ratio, profit_factor) are dimensionless.
 * - PnL aggregates (gross_profit, gross_loss) are in portfolio currency.
 */
struct HistoricalMetrics {
    // Risk-adjusted performance
    double sharpe_ratio = 0.0;
    double sortino_ratio = 0.0;
    double max_drawdown = 0.0;        // % peak-to-trough from equity curve
    double volatility = 0.0;          // annualized, % units
    double downside_deviation = 0.0;  // annualized, % units

    // Day-level win/loss statistics
    int winning_days = 0;
    int losing_days = 0;
    int total_days = 0;
    double win_rate = 0.0;  // %
    double avg_win = 0.0;   // average positive daily return, %
    double avg_loss = 0.0;  // abs(average negative daily return), %
    double best_day = 0.0;  // max daily return, %
    double worst_day = 0.0; // min daily return, %

    // Profit factor based on daily PnL
    double gross_profit = 0.0;  // sum of positive daily_pnl
    double gross_loss = 0.0;    // sum of abs(negative daily_pnl)
    double profit_factor = 0.0; // gross_profit / gross_loss

    // Trade-level stats
    int total_trades = 0;       // count of executions since inception
};

/**
 * @brief Pure calculator for since-inception live trading metrics.
 *
 * This component is stateless and performs only mathematical calculations.
 */
class LiveHistoricalMetricsCalculator {
public:
    LiveHistoricalMetricsCalculator() = default;
    ~LiveHistoricalMetricsCalculator() = default;

    /**
     * @brief Calculate all historical metrics.
     *
     * @param daily_returns_pct Daily returns in percentage points (e.g. 0.5 = 0.5%).
     * @param daily_pnl_dollars Daily PnL values in portfolio currency.
     * @param equity_values Full equity curve values (portfolio value over time).
     * @param total_annualized_return_pct Total annualized return (percentage) since inception.
     * @param total_trades_executions Total number of executions since inception.
     * @return HistoricalMetrics structure with all fields populated.
     */
    HistoricalMetrics calculate(const std::vector<double>& daily_returns_pct,
                                const std::vector<double>& daily_pnl_dollars,
                                const std::vector<double>& equity_values,
                                double total_annualized_return_pct,
                                int total_trades_executions) const;

private:
    static double calculate_mean(const std::vector<double>& values);
    static double calculate_annualized_volatility(const std::vector<double>& returns_pct);
    static double calculate_annualized_downside_deviation(const std::vector<double>& returns_pct,
                                                          double target = 0.0);
    static double calculate_max_drawdown_from_equity(const std::vector<double>& equity_values);
};

}  // namespace trade_ngin

