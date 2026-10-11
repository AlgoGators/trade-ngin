#pragma once

#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/no_value.hpp"
#include "trade_ngin/data/live_results_cell.hpp"

namespace trade_ngin {

/**
 * @brief Aggregate structure for since-inception performance metrics.
 *
 * Units:
 * - Returns-related fields (volatility, downside_deviation, avg_win, avg_loss,
 *   best_day, worst_day) are in percentage points, consistent with daily_return.
 * - Ratios (sharpe_ratio, sortino_ratio, profit_factor) are dimensionless.
 * - profit_factor has no value (kNoRatio, a NaN) while the series has no losing session; its
 *   cell of trading.live_results is then NULL (T-8D R12).
 * - total_annualized_return has no value while the series is under one year of sessions
 *   (n < K); its cell is then NULL (T-8D R40).
 * - volatility has no value under two returns; sharpe_ratio under two returns or with a
 *   volatility of 0; downside_deviation and sortino_ratio while no downside deviation can be
 *   taken (no return under 0, or under two returns). Their cells are then NULL (T-8D R12, the
 *   part that waited for the readers' optional reads). A default-constructed HistoricalMetrics
 *   carries zeros: the no-value marks are set by calculate().
 * - PnL aggregates (gross_profit, gross_loss) are in portfolio currency.
 */
struct HistoricalMetrics {
    // Risk-adjusted performance
    double sharpe_ratio = 0.0;   // mean(r) / sample sd(r) x sqrt(K), rf 0, on the grid returns;
                                 // kNoRatio with no volatility
    double sortino_ratio = 0.0;  // mean(r) x K / downside_deviation; kNoRatio with no downside
    double max_drawdown = 0.0;        // % peak-to-trough over the grid levels, peak seeded at the base
    double volatility = 0.0;          // annualized, % units; kNoRatio under two returns
    double downside_deviation = 0.0;  // annualized, % units; kNoRatio with no downside

    // Day-level win/loss statistics
    int winning_days = 0;  // grid returns above zero
    int losing_days = 0;   // grid returns below zero
    int flat_days = 0;     // grid returns of exactly zero (a carried session is one)
    int total_days = 0;  // n: the settled grid returns from the book's start (T-8D R5)
                         // = winning_days + losing_days + flat_days
    double win_rate = 0.0;  // winning_days / (winning_days + losing_days), %
    double avg_win = 0.0;   // average positive grid return, %
    double avg_loss = 0.0;  // abs(average negative grid return), %
    double best_day = 0.0;  // max grid return, %
    double worst_day = 0.0; // min grid return, %

    // Profit factor on the P&L of the grid returns (the difference of two grid levels, which
    // holds the P&L of every row folded into the return)
    double gross_profit = 0.0;  // sum of the positive grid P&L
    double gross_loss = 0.0;    // sum of abs(negative grid P&L)
    double profit_factor = 0.0; // gross_profit / gross_loss; kNoRatio with no losing session

    // Trade-level stats
    int total_trades = 0;       // count of executions since inception

    // ((1 + R)^(K / n) - 1) x 100 on the statistics grid, R from the base to the last grid level
    // and K the grid's sessions a year; kNoRatio while n < K (T-8D R40). Not one of the sixteen
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
 * @brief A futures book's statistics grid from the bars it printed (T-8D R3): the dates of
 *        `symbols_printed` (date "YYYY-MM-DD" -> the count of DISTINCT symbols of the universe
 *        that printed a bar on it) that are a Sunday to a Friday with at least
 *        kStatisticsGridMinSymbols printing, ascending. A Saturday is never a row, whatever
 *        printed on it; a date that is no date is left out. The rule
 *        LiveDataLoader::load_futures_statistics_grid applies in SQL, for a caller that holds the
 *        bars in memory (the backtest).
 */
std::vector<std::string> futures_statistics_grid_dates(
    const std::map<std::string, int>& symbols_printed);

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
 * @brief The annualised return of `n` grid returns as a fraction: growth^(K / n) - 1, `growth`
 *        being 1 + R, the last grid level over the base, and K the grid's sessions a year (the
 *        CAGR). 0 without a return or a positive K; -1 when the growth is not positive. The one
 *        formula behind the live total_annualized_return and the backtest's Calmar numerator.
 */
double grid_annualized_return(double growth, int n, double sessions_per_year);

/**
 * @brief Whether `n` grid returns are under one year of the grid's sessions (n < K). A return
 *        is not annualised under a year (T-8D R40, extended to the backtest by T-8D-2 R78): the
 *        live total_annualized_return, the backtest's annualised return and its Calmar ratio
 *        have no value while this holds. False without a positive K. K is 311.0574 for a
 *        futures series, so its first year is n = 312; an equity series' is n = 252.
 */
bool under_one_year_of_sessions(int n, double sessions_per_year);

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
     * @brief Calculate all historical metrics on the statistics series.
     *
     * @param grid The statistics series (build_statistics_series): total_days is its count of
     *        returns; total_annualized_return (from one year of sessions: n >= K; no value
     *        before), volatility, downside_deviation and the Sharpe and
     *        Sortino ratios are taken on its returns; max_drawdown on its levels from its base;
     *        winning_days, losing_days, flat_days, win_rate, avg_win, avg_loss, best_day and
     *        worst_day on its returns; gross_profit, gross_loss and profit_factor on its P&L,
     *        the differences of its levels.
     * @param sessions_per_year The sessions a year K of the grid the series is taken on; every
     *        annualised figure of the series uses this one K.
     * @param total_trades_executions Total number of executions since inception.
     * @return HistoricalMetrics structure with all fields populated; for an empty series the
     *         counts and the levels are zeros. A statistic that cannot be taken has no value
     *         (kNoRatio): profit_factor with no losing session, volatility under two returns,
     *         sharpe_ratio with no volatility, downside_deviation and sortino_ratio with no
     *         downside, total_annualized_return under a year of sessions.
     */
    HistoricalMetrics calculate(const StatisticsSeries& grid, double sessions_per_year,
                                int total_trades_executions) const;

    // The two annualised deviations of a return series, in the unit of the returns handed in
    // (percent here, fractions in the backtest calculator, which calls the same two): 0 under
    // two returns or without a positive sessions_per_year. calculate() gives the live statistic
    // no value where these return a 0 that is not a measurement.
    // Sample standard deviation (n - 1) of the returns x sqrt(sessions_per_year)
    static double calculate_annualized_volatility(const std::vector<double>& returns_pct,
                                                  double sessions_per_year);
    // sqrt(sum of min(r - target, 0)^2 over EVERY return / n) x sqrt(sessions_per_year)
    static double calculate_annualized_downside_deviation(const std::vector<double>& returns_pct,
                                                          double sessions_per_year,
                                                          double target = 0.0);

private:
    static double calculate_mean(const std::vector<double>& values);
    // The largest fall from a running peak, in percent; the peak starts at `peak_seed` (the
    // book's equity at its start, the base of the series: T-8D R9).
    static double calculate_max_drawdown_from_equity(const std::vector<double>& equity_values,
                                                     double peak_seed);
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
 * `portfolio_var`, and equities now do the same. `sharpe_ratio` is the mean grid return over
 * the same sample standard deviation, both annualised with the grid's sessions a year.
 *
 * `portfolio_var`, `var_95` and `cvar_95` are untouched: the ex-ante sigma still feeds the risk
 * gate and nothing is lost.
 *
 * `total_trades` and `flat_days` are absent because `trading.live_results` has no such
 * columns; flat_days is `total_days - winning_days - losing_days` on read.
 */
std::unordered_map<std::string, double> historical_metrics_double_columns(
    const HistoricalMetrics& m);

/**
 * @brief The columns of the block that have no value on some rows and are then stored NULL:
 *        profit_factor, with no losing session, volatility, under two returns, sharpe_ratio,
 *        with no volatility, sortino_ratio and downside_deviation, with no downside (T-8D R12),
 *        and total_annualized_return, under one year of sessions (T-8D R40), which the runner
 *        writes beside the block. The day-T
 *        INSERT leaves such a column out (historical_metrics_double_columns does not name it,
 *        and drop_statistics_without_value takes it out of the row's metrics); the Day T-1
 *        UPDATE must overwrite what the row holds, so historical_metrics_update_columns carries
 *        it as kNoRatio and the writer assigns NULL (null_cells_of_statistics_without_value).
 */
const std::vector<std::string>& historical_metrics_optional_columns();

/**
 * @brief The day-T INSERT of a statistic that has no value: every optional column of the block
 *        whose value in `metrics` is not finite leaves `metrics`, so the INSERT does not name it
 *        and its cell is NULL. Every other entry is untouched.
 */
void drop_statistics_without_value(std::unordered_map<std::string, double>& metrics);

/**
 * @brief The Day T-1 UPDATE of a statistic that has no value: every optional column of the
 *        block whose value in `updates` is not finite leaves `updates` and joins `cells` as a
 *        cell without a value, which the UPDATE assigns NULL. Every other entry is untouched.
 */
void null_cells_of_statistics_without_value(std::unordered_map<std::string, double>& updates,
                                            std::vector<LiveResultsCell>& cells);

/** @brief The integer half of the same block: winning_days, losing_days, total_days. */
std::unordered_map<std::string, int> historical_metrics_int_columns(const HistoricalMetrics& m);

/**
 * @brief The whole block as the Day T-1 UPDATE takes it: the double columns plus the three
 * integer columns widened to double (`update_live_results` takes doubles only; the three are
 * whole numbers by construction). An optional column without a value is carried as kNoRatio.
 */
std::unordered_map<std::string, double> historical_metrics_update_columns(
    const HistoricalMetrics& m);

}  // namespace trade_ngin

