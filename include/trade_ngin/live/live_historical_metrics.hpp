#pragma once

#include <functional>
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
    int total_days = 0;  // n: the settled grid returns from the book's start (T-8D R5)
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

    // ((1 + R)^(K / n) - 1) x 100 on the statistics grid, R from the base to the last grid level
    // and K the grid's sessions a year; computed for every n (T-8D-2 R74). Not one of the sixteen
    // columns of historical_metrics_*_columns: the runner writes it beside them.
    double total_annualized_return = 0.0;
};

/** @brief One stored level of a book: the account value of its live_results row of `date`. */
struct DatedLevel {
    std::string date;  ///< YYYY-MM-DD
    double level = 0.0;
};

/**
 * @brief The series the live statistics are taken on: the returns of the statistics grid from
 *        the book's base (T-8D R1, R5, R6; T-8D-2 R77).
 *
 * The grid is a statistics construct only: every stored row stays. A futures book's grid is the
 * Sunday-to-Friday dates on which enough of its universe printed a bar, an equity book's the
 * NYSE sessions; a book holding both would take the futures grid, the union of the two, with
 * the futures sessions a year (R4), and no such book is built.
 */
struct StatisticsSeries {
    double base = 0.0;                 ///< the initial capital: the level the first return is taken on
    std::vector<std::string> dates;    ///< the grid dates that are return rows, ascending
    std::vector<double> levels;        ///< the level of each: the last stored level on or before it
    std::vector<double> returns_pct;   ///< level / previous grid level - 1, in percent
    std::vector<double> pnl;           ///< level - previous grid level, in portfolio currency
    std::vector<std::string> carried;  ///< return rows with no stored row of their own (R6)

    int size() const { return static_cast<int>(returns_pct.size()); }
};

/**
 * @brief The fewest distinct symbols of a futures book's universe that must print a bar on a
 *        Sunday-to-Friday date for it to be a row of the statistics grid: a quarter of the 36
 *        (T-8D R3). The frozen sessions a year were counted with this number.
 */
inline constexpr int kStatisticsGridMinSymbols = 9;

/**
 * @brief The dates of [from, through] that `is_session(date, weekday)` accepts, ascending;
 *        `weekday` is 0 for Sunday to 6 for Saturday. An equity book's grid is this walk with
 *        its exchange calendar as the predicate.
 */
std::vector<std::string> statistics_session_dates(
    const std::string& from, const std::string& through,
    const std::function<bool(const std::string&, int)>& is_session);

/**
 * @brief Build the statistics series of a book through `through_date`.
 *
 * @param stored_levels  the book's stored levels, any order; a date before the start is ignored
 * @param grid_dates     the dates of the statistics grid, any order
 * @param book_start     the book's start (the metadata anchor); empty means the first stored row
 * @param initial_capital the base
 * @param through_date   the last date the series may reach (the last settled row, R39)
 *
 * Returns are taken on the grid's LEVELS, so the P&L of a Saturday, a holiday or a missing row
 * lands in the next grid return and nothing is dropped (R1); a grid date with no stored row
 * takes the last stored level before it (R6). The start date is the base and not a return row
 * when its level equals the initial capital, and is a return row when it differs (R5 as amended
 * by R77 (c)). A row dated off the grid has no return of its own: a series built through it ends
 * at the last grid date, which is how it carries that row's statistics (R27, R28).
 */
StatisticsSeries build_statistics_series(const std::vector<DatedLevel>& stored_levels,
                                         const std::vector<std::string>& grid_dates,
                                         const std::string& book_start, double initial_capital,
                                         const std::string& through_date);

/**
 * @brief The annualised return of a series in percent: ((1 + R)^(K / n) - 1) x 100, R from the
 *        base to the last grid level, n its returns, K the grid's sessions a year. 0 for an
 *        empty series; -100 when the level is not positive.
 */
double grid_annualized_return_pct(const StatisticsSeries& series, double sessions_per_year);

/**
 * @brief The text of the WARN the writer of the statistics logs, empty when there is nothing to
 *        say: winning + losing + flat days differ from total_days (B5A D7), or the series
 *        carries sessions with no stored row (their count, first and last).
 */
std::string statistics_days_warning(const HistoricalMetrics& m, const StatisticsSeries& series);

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
     * @param grid The statistics series: total_days is its count of returns and
     *        total_annualized_return is taken on it.
     * @param sessions_per_year The sessions a year of the grid the series is taken on.
     * @return HistoricalMetrics structure with all fields populated.
     */
    HistoricalMetrics calculate(const std::vector<double>& daily_returns_pct,
                                const std::vector<double>& daily_pnl_dollars,
                                const std::vector<double>& equity_values,
                                double total_annualized_return_pct,
                                int total_trades_executions,
                                const StatisticsSeries& grid,
                                double sessions_per_year) const;

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

/**
 * @brief The whole block as the Day T-1 UPDATE takes it: the double columns plus the three
 * integer columns widened to double (`update_live_results` takes doubles only; the three are
 * whole numbers by construction).
 */
std::unordered_map<std::string, double> historical_metrics_update_columns(
    const HistoricalMetrics& m);

/**
 * @brief The override every live runner applies after `calculate()`: when the calendar
 * trading-days count is positive, `win_rate` is recomputed as winning_days over it, in percent;
 * a count of zero or less leaves `win_rate` as calculated. `total_days` is not touched: it is
 * the grid's count of returns.
 */
void apply_trading_days_override(HistoricalMetrics& m, int trading_days_count);

}  // namespace trade_ngin

