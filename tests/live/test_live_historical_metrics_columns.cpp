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

    // The annualised return stored on that row, and the ex-ante sigma the column used to hold.
    const double stored_annualized_return = -4.702;
    const double ex_ante_portfolio_var_x100 = 21.8934;

    LiveHistoricalMetricsCalculator calc;
    const auto m = calc.calculate(returns_pct, /*pnl*/ {}, /*equity*/ {},
                                  stored_annualized_return, /*executions*/ 3);

    // Population std of the twenty returns, times sqrt(252). Hand-computed in the decisions
    // doc as 0.797689; the fixture's rounded returns give 0.7976864.
    EXPECT_NEAR(m.volatility, 0.797689, 1e-5);

    const auto doubles = historical_metrics_double_columns(m);
    EXPECT_NEAR(doubles.at("volatility"), 0.797689, 1e-5)
        << "the column is the calculator's realised return volatility";
    EXPECT_GT(std::abs(doubles.at("volatility") - ex_ante_portfolio_var_x100), 20.0)
        << "if this is ~21.89 the runner is still writing risk_eval.portfolio_var * 100 -- the "
           "ex-ante sigma of a one-stock book, which on 2026-04-15/16 was 0.0000 because the "
           "book was flat, and which ignores the fact that the book was 5 % invested";

    // And the point of the change: sharpe is now reproducible from the row it sits on.
    EXPECT_NEAR(doubles.at("sharpe_ratio"), stored_annualized_return / m.volatility, 1e-12);
    EXPECT_NEAR(doubles.at("sharpe_ratio"), -5.894563, 1e-4)
        << "the stored 2026-04-20 sharpe_ratio";
    EXPECT_NEAR(doubles.at("downside_deviation"), 1.229913, 1e-5)
        << "the stored 2026-04-20 downside_deviation, from the same series";
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
    const std::vector<double> equity = {10100.0, 9900.0, 10200.0, 10200.0, 10100.0};
    const double annualized_return_pct = 12.0;

    LiveHistoricalMetricsCalculator calc;
    const auto m = calc.calculate(returns_pct, pnl, equity, annualized_return_pct, 4);

    // mean = (1 - 2 + 3 + 0 - 1)/5 = 0.2
    // population variance = ((0.8)^2+(-2.2)^2+(2.8)^2+(-0.2)^2+(-1.2)^2)/5
    //                     = (0.64+4.84+7.84+0.04+1.44)/5 = 14.8/5 = 2.96
    // vol = sqrt(2.96) * sqrt(252) = 1.7204650... * 15.8745078... = 27.311...
    const double vol = std::sqrt(2.96) * std::sqrt(252.0);
    EXPECT_NEAR(m.volatility, vol, 1e-9);

    // downside: the returns strictly below 0 are -2 and -1; population variance over those
    // two = (4 + 1)/2 = 2.5; dd = sqrt(2.5)*sqrt(252).
    const double dd = std::sqrt(2.5) * std::sqrt(252.0);
    EXPECT_NEAR(m.downside_deviation, dd, 1e-9);

    EXPECT_NEAR(m.sharpe_ratio, annualized_return_pct / vol, 1e-12);
    EXPECT_NEAR(m.sortino_ratio, annualized_return_pct / dd, 1e-12);

    // Drawdown is tracked from a running peak SEEDED AT THE FIRST EQUITY VALUE, not at
    // initial capital: peak 10100 -> 9900 is (10100-9900)/10100*100 = 1.9801980%, and the
    // later 10200 -> 10100 leg is only 0.9803922%. The maximum is the first one. (Written
    // the other way round first, and the implementation was right: the series' own opening
    // level is the peak, so a book that starts by losing money records that loss.)
    EXPECT_NEAR(m.max_drawdown, (10100.0 - 9900.0) / 10100.0 * 100.0, 1e-9);
    EXPECT_GT(m.max_drawdown, (10200.0 - 10100.0) / 10200.0 * 100.0);

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
    EXPECT_NEAR(d.at("sharpe_ratio"), annualized_return_pct / vol, 1e-12);
    EXPECT_NEAR(d.at("gross_loss"), 300.0, 1e-12);
    EXPECT_EQ(historical_metrics_int_columns(m).at("total_days"), 5);
}
