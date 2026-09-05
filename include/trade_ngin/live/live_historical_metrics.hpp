#pragma once

#include <optional>
#include <vector>

namespace trade_ngin {

/**
 * @brief Aggregate structure for since-inception performance metrics.
 *
 * Units:
 * - Returns-related fields (volatility, downside_deviation, avg_win, avg_loss,
 *   best_day, worst_day) are in percentage points, consistent with daily_return.
 * - Ratios (sharpe_ratio, sortino_ratio, profit_factor) are dimensionless, and
 *   EMPTY where their denominator is zero. Volatility, downside deviation and
 *   the gross P&L figures are reported alongside, so an absent ratio always has
 *   its explanation in the same struct.
 * - PnL aggregates (gross_profit, gross_loss) are in portfolio currency.
 */
struct HistoricalMetrics {
    // Risk-adjusted performance.
    //
    // Both were 0.0 when their denominator was zero, which is a claim -- "no
    // return per unit of risk" -- about a quantity that has no value. A book
    // whose daily returns never varied has no Sharpe ratio; one that never had
    // a down day has no Sortino.
    std::optional<double> sharpe_ratio;   // annualised return / volatility
    std::optional<double> sortino_ratio;  // annualised return / downside deviation
    double max_drawdown = 0.0;        // % peak-to-trough from equity curve
    double volatility = 0.0;          // annualized, % units
    double downside_deviation = 0.0;  // annualized, % units

    // Day-level win/loss statistics
    int winning_days = 0;
    int losing_days = 0;
    int flat_days = 0;   // days with zero PnL — typically weekends + market holidays
    int total_days = 0;  // = winning_days + losing_days + flat_days
    double win_rate = 0.0;  // %
    double avg_win = 0.0;   // average positive daily return, %
    double avg_loss = 0.0;  // abs(average negative daily return), %
    double best_day = 0.0;  // max daily return, %
    double worst_day = 0.0; // min daily return, %

    // Profit factor based on daily PnL
    double gross_profit = 0.0;  // sum of positive daily_pnl
    double gross_loss = 0.0;    // sum of abs(negative daily_pnl)

    // gross_profit / gross_loss, and EMPTY when there is no denominator.
    //
    // A book that has not had a losing day has no profit factor -- the ratio is
    // a division by zero, not a large number. This used to be reported as
    // 999.99, a sentinel that is indistinguishable from a measurement once it
    // is in a numeric column: it averages, it sorts to the top of a leaderboard,
    // and it renders as "999.99x" on a dashboard.
    std::optional<double> profit_factor;

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

