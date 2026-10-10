// tests/live/test_live_historical_metrics_columns.cpp
//
// E2-F33 -- the equity runner never wrote the since-inception block.
//
// Fifteen columns on trading.live_results were NULL on 22/22 rows of this book and on
// 126/126 of the drift audit's, because live_equity_mean_reversion.cpp never called
// LiveHistoricalMetricsCalculator at all while both futures runners have written the block
// for Day T-1 and for day T since they were written.
//
// The wiring itself is only provable by a replay (protocol §7). What IS unit-provable, and
// what these tests pin, is the CONTRACT the two write sites share: exactly which columns
// constitute the block, and that both sites take them from one definition rather than from
// two hand-written maps that can drift.
//
// D3 (2026-09-03) -- `volatility` joined the block. It was held out when E2-F33 landed
// because the equity runner wrote `portfolio_var x 100` there, the ex-ante instrument-mix
// sigma, and filling in NULLs must not move a compared column. The lead then ruled that one
// column may not mean two things on two books: both futures runners store the REALISED
// annualised return volatility in `volatility` and keep the ex-ante sigma in `portfolio_var`,
// so equities do the same. The tests below now pin the OPPOSITE of what they pinned before --
// that the figure reaching the column is the calculator's, not the risk engine's -- and the
// last one does it on the real 2026-04-20 series.

#include <gtest/gtest.h>

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "trade_ngin/live/live_historical_metrics.hpp"

using namespace trade_ngin;

namespace {

// A HistoricalMetrics with a distinct, recognisable value in every field, so a column
// wired to the wrong member is caught by the value and not only by the key.
HistoricalMetrics distinct_metrics() {
    HistoricalMetrics m;
    m.sharpe_ratio = 1.11;
    m.sortino_ratio = 2.22;
    m.max_drawdown = 3.33;
    m.volatility = 4.44;
    m.downside_deviation = 5.55;
    m.winning_days = 6;
    m.losing_days = 7;
    m.flat_days = 8;
    m.total_days = 9;
    m.win_rate = 10.10;
    m.avg_win = 11.11;
    m.avg_loss = 12.12;
    m.best_day = 13.13;
    m.worst_day = -14.14;
    m.gross_profit = 15.15;
    m.gross_loss = 16.16;
    m.profit_factor = 17.17;
    m.total_trades = 18;
    return m;
}

// A statistics series of `n` flat sessions on a base of 100: the grid argument of calculate()
// for a test that pins a statistic the grid does not feed.
StatisticsSeries flat_grid(int n) {
    std::vector<DatedLevel> levels;
    std::vector<std::string> dates;
    for (int i = 0; i < n; ++i) {
        const std::string date = "2026-01-" + std::string(i + 2 < 10 ? "0" : "") + std::to_string(i + 2);
        levels.push_back({date, 100.0});
        dates.push_back(date);
    }
    return build_statistics_series(levels, dates, "2026-01-01", 100.0, "2026-01-31");
}

// The statistics series whose grid returns are `returns_pct`, on a base of 100: one stored
// level per session from 2026-01-02, the start row flat at the base.
StatisticsSeries grid_of_returns(const std::vector<double>& returns_pct) {
    std::vector<DatedLevel> levels;
    std::vector<std::string> dates;
    double level = 100.0;
    for (size_t i = 0; i < returns_pct.size(); ++i) {
        const int day = static_cast<int>(i) + 2;
        const std::string date = "2026-01-" + std::string(day < 10 ? "0" : "") + std::to_string(day);
        level *= 1.0 + returns_pct[i] / 100.0;
        levels.push_back({date, level});
        dates.push_back(date);
    }
    return build_statistics_series(levels, dates, "2026-01-01", 100.0, "2026-01-31");
}
}  // namespace

TEST(HistoricalMetricsColumns, TheBlockIsTheFifteenNullColumnsPlusVolatility) {
    const auto m = distinct_metrics();
    const auto doubles = historical_metrics_double_columns(m);
    const auto ints = historical_metrics_int_columns(m);

    std::set<std::string> keys;
    for (const auto& [k, v] : doubles) keys.insert(k);
    for (const auto& [k, v] : ints) keys.insert(k);

    const std::set<std::string> expected = {
        "volatility",   "sharpe_ratio",  "sortino_ratio", "max_drawdown",
        "downside_deviation", "win_rate", "avg_win",      "avg_loss",
        "profit_factor", "best_day",     "worst_day",     "gross_profit",
        "gross_loss",   "winning_days",  "losing_days",   "total_days"};

    EXPECT_EQ(keys, expected)
        << "the set of columns the equity runner fills in changed; fifteen of these were "
           "NULL on every equity live_results row before E2-F33 and `volatility` carried the "
           "wrong quantity until D3, and both write sites (the Day T-1 UPDATE and the day-T "
           "INSERT) take the set from here";
    EXPECT_EQ(doubles.size(), 13u);
    EXPECT_EQ(ints.size(), 3u);
}

TEST(HistoricalMetricsColumns, VolatilityIsTheCalculatorsFigureAndNotTheRiskEngines) {
    const auto m = distinct_metrics();
    const auto doubles = historical_metrics_double_columns(m);

    ASSERT_EQ(doubles.count("volatility"), 1u)
        << "D3: trading.live_results.volatility carries the REALISED annualised return "
           "volatility on both futures books, and equities must not mean something else by "
           "the same column";
    EXPECT_DOUBLE_EQ(doubles.at("volatility"), m.volatility)
        << "the column must carry HistoricalMetrics::volatility -- the calculator's figure -- "
           "and not risk_eval.portfolio_var * 100, the ex-ante instrument-mix sigma";

    // `portfolio_var`, `var_95` and `cvar_95` are NOT this block's business: they keep the
    // ex-ante figure, which is what the risk gate reads.
    EXPECT_EQ(doubles.count("portfolio_var"), 0u);
    EXPECT_EQ(doubles.count("var_95"), 0u);
    EXPECT_EQ(doubles.count("cvar_95"), 0u);

    // These two have no place on the table at all.
    EXPECT_EQ(doubles.count("total_trades"), 0u);
    EXPECT_EQ(doubles.count("flat_days"), 0u);
    EXPECT_EQ(historical_metrics_int_columns(m).count("total_trades"), 0u);
    EXPECT_EQ(historical_metrics_int_columns(m).count("flat_days"), 0u);
}

// The real 2026-04-20 row of EQUITY_MR_PORTFOLIO, from
// reports/LEAD_B5A_METRICS_DECISIONS.md: 20 stored daily returns in percent, five of them the
// zeros of the pre-trading carry rows. This is the fixture the decision was taken on, so it is
// the fixture the column is pinned against.
TEST(HistoricalMetricsColumns, TheRealAprilTwentiethRowCarriesZeroPointEightNotTwentyTwo) {
    const std::vector<double> returns_pct = {
        0.0,       0.0,       0.0,       0.0,       0.0,       -0.092948, 0.063900,
        -0.096632, -0.004166, -0.065039, 0.0,       0.0,       -0.124015, -0.091636,
        -0.001935, 0.0,       0.075430,  0.0,       0.0,       -0.044904};
    ASSERT_EQ(returns_pct.size(), 20u);

    // The ex-ante sigma the column used to hold.
    const double ex_ante_portfolio_var_x100 = 21.8934;

    LiveHistoricalMetricsCalculator calc;
    const auto m = calc.calculate(returns_pct, /*pnl*/ {}, /*executions*/ 3,
                                  grid_of_returns(returns_pct), 252.0);

    // Sample sd of the twenty returns, times sqrt(252): the population figure the decisions
    // doc hand-computed, 0.797689 (0.7976864 on the fixture's rounded returns), times
    // sqrt(20 / 19) = 0.818409.
    EXPECT_NEAR(m.volatility, 0.7976864 * std::sqrt(20.0 / 19.0), 1e-5);
    EXPECT_NEAR(m.volatility, 0.818409, 1e-5);

    const auto doubles = historical_metrics_double_columns(m);
    EXPECT_NEAR(doubles.at("volatility"), 0.818409, 1e-5)
        << "the column is the calculator's realised return volatility";
    EXPECT_GT(std::abs(doubles.at("volatility") - ex_ante_portfolio_var_x100), 20.0)
        << "if this is ~21.89 the runner is still writing risk_eval.portfolio_var * 100 -- the "
           "ex-ante sigma of a one-stock book, which on 2026-04-15/16 was 0.0000 because the "
           "book was flat, and which ignores the fact that the book was 5 % invested";

    // The Sharpe ratio is the mean return over the same sample sd, annualised with the same
    // sessions a year: mean -0.0190973 x 252 / 0.818409 = -5.880320 (the row as stored on
    // 2026-04-20 divided a calendar-day annualised return by the population figure: -5.894563).
    double sum = 0.0;
    for (double r : returns_pct) sum += r;
    EXPECT_NEAR(doubles.at("sharpe_ratio"), sum / 20.0 * 252.0 / m.volatility, 1e-9);
    EXPECT_NEAR(doubles.at("sharpe_ratio"), -5.880320, 1e-5);
    // Eight of the twenty returns are below zero; their squares sum to 0.048021 and are
    // averaged over all twenty days: sqrt(0.048021 / 20) * sqrt(252) = 0.777863. (The row as
    // stored on 2026-04-20 divided by the eight losing days and held 1.229913.)
    EXPECT_NEAR(doubles.at("downside_deviation"), 0.777863, 1e-5)
        << "the 2026-04-20 downside_deviation, from the same series, over all twenty days";
}

TEST(HistoricalMetricsColumns, EveryColumnCarriesItsOwnMemberAndNotItsNeighbours) {
    const auto m = distinct_metrics();
    const auto d = historical_metrics_double_columns(m);
    const auto i = historical_metrics_int_columns(m);

    EXPECT_DOUBLE_EQ(d.at("sharpe_ratio"), 1.11);
    EXPECT_DOUBLE_EQ(d.at("sortino_ratio"), 2.22);
    EXPECT_DOUBLE_EQ(d.at("max_drawdown"), 3.33);
    EXPECT_DOUBLE_EQ(d.at("downside_deviation"), 5.55);
    EXPECT_DOUBLE_EQ(d.at("win_rate"), 10.10);
    EXPECT_DOUBLE_EQ(d.at("avg_win"), 11.11);
    EXPECT_DOUBLE_EQ(d.at("avg_loss"), 12.12);
    EXPECT_DOUBLE_EQ(d.at("profit_factor"), 17.17);
    EXPECT_DOUBLE_EQ(d.at("best_day"), 13.13);
    EXPECT_DOUBLE_EQ(d.at("worst_day"), -14.14);
    EXPECT_DOUBLE_EQ(d.at("gross_profit"), 15.15);
    EXPECT_DOUBLE_EQ(d.at("gross_loss"), 16.16);
    EXPECT_EQ(i.at("winning_days"), 6);
    EXPECT_EQ(i.at("losing_days"), 7);
    EXPECT_EQ(i.at("total_days"), 9);
}

// The series behind the block, hand-computed end to end (B-6's "unit on the metrics from a
// known series"). Every number below is derived on paper from the inputs, not read off the
// implementation, so a change to the definitions has to be argued rather than absorbed.
TEST(HistoricalMetricsColumns, KnownSeriesProducesHandComputedColumns) {
    // Five daily returns in PERCENT -- the units trading.live_results.daily_return stores,
    // which is why neither runner scales the loaded series by 100.
    const std::vector<double> returns_pct = {1.0, -2.0, 3.0, 0.0, -1.0};
    const std::vector<double> pnl = {100.0, -200.0, 300.0, 0.0, -100.0};

    LiveHistoricalMetricsCalculator calc;
    const auto m = calc.calculate(returns_pct, pnl, 4, grid_of_returns(returns_pct), 252.0);

    // mean = (1 - 2 + 3 + 0 - 1)/5 = 0.2
    // sample variance = ((0.8)^2+(-2.2)^2+(2.8)^2+(-0.2)^2+(-1.2)^2)/(5 - 1)
    //                 = (0.64+4.84+7.84+0.04+1.44)/4 = 14.8/4 = 3.7
    // vol = sqrt(3.7) * sqrt(252) = 1.9235384... * 15.8745078... = 30.535...
    const double vol = std::sqrt(3.7) * std::sqrt(252.0);
    EXPECT_NEAR(m.volatility, vol, 1e-9);

    // downside: the returns strictly below 0 are -2 and -1; their squares, 4 + 1 = 5, are
    // averaged over all five days = 1; dd = sqrt(1)*sqrt(252) = 15.874507866.
    const double dd = std::sqrt(5.0 / 5.0) * std::sqrt(252.0);
    EXPECT_NEAR(m.downside_deviation, dd, 1e-9);
    EXPECT_NEAR(m.downside_deviation, 15.874507866, 1e-9);

    // Sharpe = mean / sample sd x sqrt(K) = 0.2 / sqrt(3.7) x sqrt(252) = 1.650553;
    // Sortino = mean x K / downside = 0.2 x 252 / 15.874507866 = 3.174902.
    EXPECT_NEAR(m.sharpe_ratio, 0.2 / std::sqrt(3.7) * std::sqrt(252.0), 1e-9);
    EXPECT_NEAR(m.sharpe_ratio, 1.650553, 1e-6);
    EXPECT_NEAR(m.sortino_ratio, 0.2 * 252.0 / dd, 1e-9);
    EXPECT_NEAR(m.sortino_ratio, 3.174902, 1e-6);

    // Drawdown over the grid levels 101, 98.98, 101.9494, 101.9494, 100.929906 with the peak
    // seeded at the base 100: the peak 101 to 98.98 is the -2 percent day, 2.0; the later
    // 101.9494 to 100.929906 leg is 1.0. The maximum is the first one.
    EXPECT_NEAR(m.max_drawdown, 2.0, 1e-9);

    EXPECT_EQ(m.winning_days, 2);
    EXPECT_EQ(m.losing_days, 2);
    EXPECT_EQ(m.flat_days, 1);
    EXPECT_EQ(m.total_days, 5);
    EXPECT_NEAR(m.win_rate, 2.0 / 5.0 * 100.0, 1e-12);
    EXPECT_NEAR(m.avg_win, (1.0 + 3.0) / 2.0, 1e-12);
    EXPECT_NEAR(m.avg_loss, (2.0 + 1.0) / 2.0, 1e-12);
    EXPECT_DOUBLE_EQ(m.best_day, 3.0);
    EXPECT_DOUBLE_EQ(m.worst_day, -2.0);
    EXPECT_DOUBLE_EQ(m.gross_profit, 400.0);
    EXPECT_DOUBLE_EQ(m.gross_loss, 300.0);
    EXPECT_NEAR(m.profit_factor, 400.0 / 300.0, 1e-12);
    EXPECT_EQ(m.total_trades, 4);

    // And the columns carry exactly those numbers.
    const auto d = historical_metrics_double_columns(m);
    EXPECT_NEAR(d.at("sharpe_ratio"), 0.2 * 252.0 / vol, 1e-9);
    EXPECT_NEAR(d.at("gross_loss"), 300.0, 1e-12);
    EXPECT_EQ(historical_metrics_int_columns(m).at("total_days"), 5);
}

// The Day T-1 UPDATE takes one map of doubles: the thirteen double columns and the three
// integer columns widened. All three live runners build it here.
TEST(HistoricalMetricsColumns, TheUpdateBlockIsTheDoublesPlusTheThreeWidenedIntegers) {
    const HistoricalMetrics m = distinct_metrics();
    const auto update = historical_metrics_update_columns(m);
    const auto doubles = historical_metrics_double_columns(m);
    const auto ints = historical_metrics_int_columns(m);

    EXPECT_EQ(update.size(), doubles.size() + ints.size());
    EXPECT_EQ(update.size(), 16u);
    for (const auto& [column, value] : doubles) {
        ASSERT_EQ(update.count(column), 1u) << column;
        EXPECT_DOUBLE_EQ(update.at(column), value) << column;
    }
    for (const auto& [column, value] : ints) {
        ASSERT_EQ(update.count(column), 1u) << column;
        EXPECT_DOUBLE_EQ(update.at(column), static_cast<double>(value)) << column;
    }
    EXPECT_EQ(update.count("total_trades"), 0u);
    EXPECT_EQ(update.count("flat_days"), 0u);
}

// The override the runners apply after calculate(): win_rate is winning_days over the calendar
// trading-days count, in percent, to the last bit of the expression the runners carried inline.
// total_days is the grid's count of returns (T-8D R5) and is not the override's to set.
TEST(HistoricalMetricsColumns, TheTradingDaysOverrideRecomputesWinRateAndLeavesTotalDays) {
    HistoricalMetrics m = distinct_metrics();
    apply_trading_days_override(m, 211);
    EXPECT_EQ(m.total_days, 9);
    EXPECT_EQ(m.win_rate, static_cast<double>(6) / static_cast<double>(211) * 100.0);
    EXPECT_EQ(m.winning_days, 6);
    EXPECT_EQ(m.losing_days, 7);
}

TEST(HistoricalMetricsColumns, ANonPositiveTradingDaysCountLeavesWinRateAsCalculated) {
    HistoricalMetrics zero = distinct_metrics();
    apply_trading_days_override(zero, 0);
    EXPECT_EQ(zero.total_days, 9);
    EXPECT_EQ(zero.win_rate, 10.10);

    HistoricalMetrics negative = distinct_metrics();
    apply_trading_days_override(negative, -1);
    EXPECT_EQ(negative.total_days, 9);
    EXPECT_EQ(negative.win_rate, 10.10);
}
