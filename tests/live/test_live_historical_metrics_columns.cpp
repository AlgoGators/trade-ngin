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
// constitute the block, that both sites take them from one definition rather than from two
// hand-written maps that can drift, and -- the one that would silently move a compared
// column -- that `volatility` is NOT in it. The equity runner writes the portfolio-VaR proxy
// into `volatility`; the futures runner overwrites it with the return volatility. Filling in
// NULLs must not change a column that already carries a value.

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

TEST(HistoricalMetricsColumns, TheBlockIsExactlyTheFifteenColumnsThatWereNull) {
    const auto m = distinct_metrics();
    const auto doubles = historical_metrics_double_columns(m);
    const auto ints = historical_metrics_int_columns(m);

    std::set<std::string> keys;
    for (const auto& [k, v] : doubles) keys.insert(k);
    for (const auto& [k, v] : ints) keys.insert(k);

    const std::set<std::string> expected = {
        "sharpe_ratio", "sortino_ratio", "max_drawdown", "downside_deviation",
        "win_rate",     "avg_win",       "avg_loss",     "profit_factor",
        "best_day",     "worst_day",     "gross_profit", "gross_loss",
        "winning_days", "losing_days",   "total_days"};

    EXPECT_EQ(keys, expected)
        << "the set of columns the equity runner fills in changed; every one of these was "
           "NULL on every equity live_results row before E2-F33, and both write sites (the "
           "Day T-1 UPDATE and the day-T INSERT) take the set from here";
    EXPECT_EQ(doubles.size(), 12u);
    EXPECT_EQ(ints.size(), 3u);
}

TEST(HistoricalMetricsColumns, VolatilityIsDeliberatelyNotInTheBlock) {
    const auto doubles = historical_metrics_double_columns(distinct_metrics());
    EXPECT_EQ(doubles.count("volatility"), 0u)
        << "trading.live_results.volatility already carries the portfolio-VaR proxy on every "
           "equity row and the chain gate compares it. HistoricalMetrics::volatility is the "
           "annualized RETURN volatility -- a different quantity. Putting it in this map "
           "silently moves a populated column while claiming to fill in NULLs.";
    // The same reasoning, one column over: these two have no place on the table at all.
    EXPECT_EQ(doubles.count("total_trades"), 0u);
    EXPECT_EQ(doubles.count("flat_days"), 0u);
    EXPECT_EQ(historical_metrics_int_columns(distinct_metrics()).count("total_trades"), 0u);
    EXPECT_EQ(historical_metrics_int_columns(distinct_metrics()).count("flat_days"), 0u);
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
