// The backtest statistics of migration 037 (LOOP_SPEC v6.2 sections 7.4 and 10; T-8b commit
// (1b)): on backtest.results, the worst day with its date and symbol, the three statistics on
// the sizing capital, the monthly skew and tail ratio, the calendar-year returns with the losing
// years, and the count of the account's STRATEGY fills.
//
// ONE IMPLEMENTATION with the live columns of migration 036: every figure is
// live_statistics_columns() on the run's statistics series (the grid, the sessions a year K and
// the sample deviation of BacktestMetricsCalculator), and the fill count is the trade
// statistics' own account_fills. The live cells are in PERCENT; a backtest stores FRACTIONS, so
// a return, a volatility and a drawdown are divided by 100 here and nowhere else.

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/live_results_cell.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"

namespace trade_ngin {

/// The thirteen cells of migration 037 on backtest.results. A field with no value is a NULL cell.
struct BacktestStatisticsColumns {
    std::optional<double> worst_day;  ///< the smallest grid return, a fraction
    std::optional<std::string> worst_day_date;
    std::optional<std::string> worst_day_symbol;
    std::optional<double> max_drawdown_sizing;  ///< a fraction of the sizing capital, positive
    std::optional<double> volatility_sizing;    ///< a fraction, annualised
    std::optional<double> worst_day_sizing;     ///< a fraction of the sizing capital
    std::optional<std::string> worst_day_sizing_date;
    std::optional<std::string> worst_day_sizing_symbol;
    std::optional<double> monthly_skew;
    std::optional<double> monthly_tail_ratio;
    /// {"YYYY": {"return": fraction, "partial": bool}}
    std::optional<nlohmann::json> calendar_year_returns;
    std::optional<int> losing_years;
    std::optional<int> total_strategy_fills;

    int sizing_returns = 0;   ///< the count of returns on the sizing capital (not a column)
    int complete_months = 0;  ///< the count of complete months (not a column)
};

/// What the statistics of migration 037 read beside the equity curve and the executions: the
/// run's stored per-symbol P&L cells and its stored sizing capitals. Both empty: the worst day's
/// symbol and the three sizing-capital statistics are NULL.
struct BacktestStatisticsInputs {
    /// One entry per symbol and stored date, summed over the sleeves: the cells of the
    /// backtest.final_positions rows of the dates after the warm-up.
    std::vector<SymbolDayPnl> symbol_pnl;
    /// risk_detail.sizing_capital of the equity-curve rows that carry one; none on an equity run.
    std::vector<DatedCapital> capitals;
};

/**
 * @brief The statistics of migration 037 on a run's statistics series, but for
 *        total_strategy_fills, which the caller sets from the run's account fills.
 *
 * @param series            the run's statistics series: the grid returns after the warm-up
 * @param sessions_per_year the K of the series' grid
 * @param base_date         the date of the base row (the first row after the warm-up): the level
 *                          the first return is taken on; its own P&L is in no return
 * @param through           the date of the run's last equity-curve row
 * @param inputs            the stored per-symbol P&L and the stored sizing capitals
 *
 * live_statistics_columns() with the base row's date as the book's start (a month or a year is
 * complete or full when it begins after that date and ends on or before `through`), each
 * percent figure divided by 100. worst_day is the smallest grid return.
 */
BacktestStatisticsColumns backtest_statistics_columns(const StatisticsSeries& series,
                                                      double sessions_per_year,
                                                      const std::string& base_date,
                                                      const std::string& through,
                                                      const BacktestStatisticsInputs& inputs);

/// The thirteen as typed cells, in the migration's order. A number is written as the text that
/// reads back as the same double (live_results_number, the format of the 036 cells).
std::vector<LiveResultsCell> backtest_statistics_cells(const BacktestStatisticsColumns& columns);

/// The columns as one log line (a NULL cell prints "null").
std::string backtest_statistics_log_line(const BacktestStatisticsColumns& columns);

/// Rolls step 4 (ruling 18, migration 040): the run's two roll split totals as typed cells
/// (roll_bar_moves, roll_gap_values), only those with a value: none with per_contract_rolls off,
/// so the results INSERT names neither column and is today's statement.
std::vector<LiveResultsCell> roll_split_summary_cells(const std::optional<double>& roll_bar_moves,
                                                      const std::optional<double>& roll_gap_values);

/**
 * @brief The sizing capitals a run stores: risk_detail.sizing_capital of each equity-curve row
 *        that carries a risk_detail, dated by the row's UTC date.
 *
 * @param curve       the equity curve, one entry a row
 * @param risk_detail per row, its risk_detail object as text (empty: a NULL cell); empty
 *                    altogether on a run that stores none
 *
 * Read from the text the row stores, so E_t is the stored cell's own figure. A text that is no
 * object or carries no numeric sizing_capital gives no capital for its row.
 */
std::vector<DatedCapital> stored_sizing_capitals(
    const std::vector<std::pair<Timestamp, double>>& curve,
    const std::vector<std::string>& risk_detail);

}  // namespace trade_ngin
