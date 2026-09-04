#pragma once

#include <string>
#include <unordered_map>
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

/**
 * @brief The since-inception block as `trading.live_results` columns (E2-F33).
 *
 * One definition of "which columns ARE the historical-metrics block", so the two sites that
 * write it -- the Day T-1 UPDATE and the day-T INSERT -- cannot drift apart and drop a
 * column on one path only. Fifteen columns in total, every one of which was NULL on every
 * equity row before E2-F33.
 *
 * `volatility` IS one of them (D3, 2026-09-03). It was held out when the block first landed
 * because the equity runner wrote `portfolio_var x 100` -- the ex-ante instrument-mix sigma --
 * into that column and the chain gate compared it, so filling in NULLs must not have moved it.
 * The lead then ruled the two books may not carry two meanings in one column: both futures
 * runners store the REALISED annualised return volatility here and keep the ex-ante sigma in
 * `portfolio_var`, and equities now do the same. It also makes `sharpe_ratio` reproducible
 * from the row it is written on -- `sharpe = total_annualized_return / volatility` -- which it
 * was not while the denominator lived nowhere.
 *
 * `portfolio_var`, `var_95` and `cvar_95` are untouched: the ex-ante sigma still feeds the risk
 * gate and nothing is lost.
 *
 * `total_trades` and `flat_days` are absent because `trading.live_results` has no such
 * columns; flat_days is `total_days - winning_days - losing_days` on read.
 */
std::unordered_map<std::string, double> historical_metrics_double_columns(
    const HistoricalMetrics& m);

/** @brief The integer half of the same block: winning_days, losing_days, total_days. */
std::unordered_map<std::string, int> historical_metrics_int_columns(const HistoricalMetrics& m);

}  // namespace trade_ngin

