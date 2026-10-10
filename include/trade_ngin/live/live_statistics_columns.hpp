// The live statistics of migration 030 (LOOP_SPEC v6.2 sections 7.4 and 10; T-8a commit (12b)):
// the worst day's date and symbol, the three statistics on the sizing capital, the monthly skew
// and tail ratio, the calendar-year returns with the losing years, and the three fill counts.
//
// Every one is taken on the statistics series of live_historical_metrics.hpp (the same grid, the
// same sessions a year K, through the last settled row), in the same block of the runners, and
// is written to the Day T-1 row and to the row the run writes. A statistic that has no value is
// NULL: the day's INSERT leaves the column out and the Day T-1 refresh writes NULL.
//
// Units: a return, a volatility and a drawdown are in PERCENT, as the account twins are
// (worst_day, volatility, max_drawdown); the skew and the tail ratio are ratios.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/live_results_cell.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"

namespace trade_ngin {

/// One symbol's stored P&L cells of one date, summed over the book's sleeves
/// (trading.positions): the day's realised P&L and the unrealised P&L LEVEL the row carries. A
/// futures row books its whole daily move as realised and carries 0 unrealised; an equity row
/// carries the open position's unrealised P&L since entry.
struct SymbolDayPnl {
    std::string date;  ///< YYYY-MM-DD
    std::string symbol;
    double realized = 0.0;
    double unrealized_level = 0.0;
};

/// The sizing capital E_t a row was sized on: risk_detail.sizing_capital of the row of `date`.
struct DatedCapital {
    std::string date;  ///< YYYY-MM-DD
    double capital = 0.0;
};

/**
 * @brief The symbol whose P&L over the stored rows dated in (after, through] is the most
 *        negative; none when no symbol lost over them.
 *
 * A symbol's P&L over the rows is the sum of its realised P&L on them plus the change of its
 * unrealised level from the last stored date on or before `after` to the last stored date on or
 * before `through` (a symbol with no row on a date carries 0 there). The rows of a grid return
 * that folds a Saturday, a holiday or a missing row are taken together; a symbol two sleeves
 * hold is the book's (the cells are summed over the sleeves). An empty `after` means from the
 * first stored row. Equal losses: the first symbol in text order.
 */
std::optional<std::string> most_negative_symbol(const std::vector<SymbolDayPnl>& symbol_pnl,
                                                const std::string& after,
                                                const std::string& through);

/// The returns on the sizing capital, r^s = the grid return's P&L / E_t x 100, in percent.
struct SizingReturns {
    std::vector<std::string> dates;
    std::vector<double> returns_pct;
    std::vector<size_t> grid_index;  ///< each return's index in the statistics series
};

/**
 * @brief The series of returns on the sizing capital (LOOP_SPEC section 10).
 *
 * E_t of a grid return is the sizing capital of the last stored row on or before its grid date
 * that carries one: the grid row's own where it has one, else the row before it (a Sunday row
 * that sized nothing reads Saturday's, which the recursion of section 3.1 leaves equal to its
 * own). A Saturday or holiday row folded into the return adds its P&L to the numerator and
 * never supplies the denominator of a grid row that has its own. The series starts at the first
 * grid date that has such a capital: a row written before risk_detail existed has none.
 */
SizingReturns sizing_capital_returns(const StatisticsSeries& series,
                                     const std::vector<DatedCapital>& capitals);

/// One complete calendar month of the series: "YYYY-MM" and its compounded return in percent.
struct MonthlyReturn {
    std::string month;
    double return_pct = 0.0;
};

/**
 * @brief The compounded returns of the COMPLETE calendar months of the series.
 *
 * A month is complete when its first calendar day is after the book's start and its last
 * calendar day is on or before `through`: the month the book starts in is partial (also when the
 * start is the 1st: the start date is the base, not a return), and the month of the last settled
 * date is partial until that date is its last day. A month's return is the product of
 * (1 + r / 100) over its grid returns, less 1.
 */
std::vector<MonthlyReturn> complete_month_returns(const StatisticsSeries& series,
                                                  const std::string& book_start,
                                                  const std::string& through);

/**
 * @brief The sample skewness, the adjusted Fisher-Pearson coefficient
 *        G1 = n / ((n - 1)(n - 2)) x sum(((x - mean) / s)^3), s the sample standard deviation
 *        (n - 1). No value under 3 observations or with s = 0.
 */
std::optional<double> sample_skewness(const std::vector<double>& values);

/**
 * @brief The tail ratio: the mean of the values at or above the 95th percentile over the
 *        absolute mean of the values at or below the 5th percentile, the percentiles by linear
 *        interpolation between order statistics (overlay::percentile). No value for an empty
 *        input or a lower-tail mean of 0.
 */
std::optional<double> tail_ratio(const std::vector<double>& values);

/**
 * @brief The calendar-year returns of the series as {"YYYY": {"return": percent, "partial":
 *        bool}}, each year compounded from its grid returns, and the count of FULL years with a
 *        negative return. A year is full when its 1 January is after the book's start and its
 *        31 December is on or before `through`; a partial first or last year is shown and never
 *        counted.
 */
nlohmann::json calendar_year_returns(const StatisticsSeries& series, const std::string& book_start,
                                     const std::string& through, int* losing_years);

/// The cells of migration 030's statistics. A field with no value is a NULL cell.
struct LiveStatisticsColumns {
    std::optional<std::string> worst_day_date;
    std::optional<std::string> worst_day_symbol;
    std::optional<double> max_drawdown_sizing;
    std::optional<double> volatility_sizing;
    std::optional<double> worst_day_sizing;
    std::optional<std::string> worst_day_sizing_date;
    std::optional<std::string> worst_day_sizing_symbol;
    std::optional<double> monthly_skew;
    std::optional<double> monthly_tail_ratio;
    std::optional<nlohmann::json> calendar_year_returns;
    std::optional<int> losing_years;
    std::optional<int> total_trades;
    std::optional<int> total_strategy_fills;
    std::optional<int> total_roll_fills;

    int sizing_returns = 0;   ///< the count of returns on the sizing capital (not a column)
    int complete_months = 0;  ///< the count of complete months (not a column)
};

/// The fewest complete months the monthly skew and tail ratio are computed on.
inline constexpr int kMonthlyStatisticsMinMonths = 12;

/**
 * @brief The statistics of migration 030 on a series, but for the three fill counts, which the
 *        caller sets from the book's executions.
 *
 * @param series            the statistics series through the last settled row
 * @param sessions_per_year the K of the series' grid
 * @param book_start        the book's start (the metadata anchor); empty: the first return's date
 * @param through           the last settled date the series was built through
 * @param symbol_pnl        the book's stored per-symbol P&L cells from its start through `through`
 * @param capitals          the stored sizing capitals; empty on a book that stores none (the
 *                          equity book), whose three sizing-capital statistics stay NULL
 *
 * The worst day: the first smallest grid return (HistoricalMetrics::worst_day is its value), its
 * date, and most_negative_symbol over the rows folded into it. On the sizing capital: fewer than
 * 2 returns leave the three statistics NULL; volatility_sizing = sample sd(r^s) x sqrt(K);
 * max_drawdown_sizing = the largest fall of the running SUM of r^s from its running peak (the
 * peak starts at 0), in percentage points, positive; the worst day as on the account. The
 * monthly skew and tail ratio need kMonthlyStatisticsMinMonths complete months.
 */
LiveStatisticsColumns live_statistics_columns(const StatisticsSeries& series,
                                              double sessions_per_year,
                                              const std::string& book_start,
                                              const std::string& through,
                                              const std::vector<SymbolDayPnl>& symbol_pnl,
                                              const std::vector<DatedCapital>& capitals);

/**
 * @brief Set the three fill counts from the book's stored executions (LOOP_SPEC section 10; HD
 *        2026-10-10): total_trades = round trips, total_strategy_fills = the ACCOUNT's fills
 *        after netting between sleeves, total_roll_fills = the ROLL legs. One definition with
 *        the backtest: BacktestMetricsCalculator::account_fill_counts. Returns the reason when
 *        the counts could not be taken (the helper threw); the three then stay NULL.
 */
std::string set_fill_counts(LiveStatisticsColumns& columns,
                            const std::vector<ExecutionReport>& executions);

/// The columns as one log line (a NULL cell prints "null").
std::string live_statistics_log_line(const LiveStatisticsColumns& columns);

/// A number as a cell's text: ten significant digits, the format of the OVERLAY log line. No
/// value for a number that is not finite.
std::optional<std::string> live_results_number(double value);

/// The fourteen statistics as typed cells, in the migration's order.
std::vector<LiveResultsCell> live_statistics_cells(const LiveStatisticsColumns& columns);

}  // namespace trade_ngin
