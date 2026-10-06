#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/backtest/backtest_types.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

ExecutionReport make_exec(const std::string& symbol, Side side, double qty, double price,
                           Timestamp ts, double tx_costs = 0.0) {
    ExecutionReport exec;
    exec.order_id = "O";
    exec.exec_id = "E";
    exec.symbol = symbol;
    exec.side = side;
    exec.filled_quantity = Quantity(qty);
    exec.fill_price = Price(price);
    exec.fill_time = ts;
    exec.commissions_fees = Decimal(tx_costs);
    exec.implicit_price_impact = Decimal(0.0);
    exec.slippage_market_impact = Decimal(0.0);
    exec.total_transaction_costs = Decimal(tx_costs);
    return exec;
}

std::vector<std::pair<Timestamp, double>> linear_equity_curve(int days, double start, double step) {
    std::vector<std::pair<Timestamp, double>> curve;
    for (int i = 0; i < days; ++i) {
        curve.emplace_back(date_at(2026, 1, 1) + std::chrono::hours(24 * i), start + i * step);
    }
    return curve;
}

}  // namespace

class BacktestMetricsCalculatorTest : public TestBase {
protected:
    BacktestMetricsCalculator calc_;
};

TEST_F(BacktestMetricsCalculatorTest, TotalReturnComputesPercentageChange) {
    EXPECT_DOUBLE_EQ(calc_.calculate_total_return(100.0, 110.0), 0.10);
    EXPECT_DOUBLE_EQ(calc_.calculate_total_return(100.0, 90.0), -0.10);
}

TEST_F(BacktestMetricsCalculatorTest, TotalReturnNonPositiveStartIsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_total_return(0.0, 110.0), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_total_return(-1.0, 110.0), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, AnnualizedReturnScalesBy252OverDays) {
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, 252), 0.10);
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, 126), 0.20);
}

TEST_F(BacktestMetricsCalculatorTest, AnnualizedReturnNonPositiveDaysIsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, 0), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, -5), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, ReturnsFromEquitySingleEntryEmpty) {
    auto curve = linear_equity_curve(1, 100.0, 1.0);
    auto returns = calc_.calculate_returns_from_equity(curve);
    EXPECT_TRUE(returns.empty());
}

TEST_F(BacktestMetricsCalculatorTest, ReturnsFromEquitySkipsZeroOrNegativePriorEquity) {
    std::vector<std::pair<Timestamp, double>> curve;
    curve.emplace_back(date_at(2026, 1, 1), 0.0);
    curve.emplace_back(date_at(2026, 1, 2), 100.0);
    curve.emplace_back(date_at(2026, 1, 3), 110.0);
    auto returns = calc_.calculate_returns_from_equity(curve);
    ASSERT_EQ(returns.size(), 1u);  // first transition is skipped
    EXPECT_DOUBLE_EQ(returns[0], 0.10);
}

TEST_F(BacktestMetricsCalculatorTest, VolatilityEmptyIsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_volatility({}), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, VolatilityConstantSeriesIsZeroNotNaN) {
    // Regression: the one-pass E[r^2] - mean^2 formula cancels catastrophically
    // and rounds to a slightly negative variance for this input, so sqrt()
    // returned NaN instead of 0.
    auto vol = calc_.calculate_volatility({0.007, 0.007, 0.007, 0.007, 0.007});
    EXPECT_DOUBLE_EQ(vol, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SharpeConstantSeriesIsUndefinedNotNaN) {
    // With NaN volatility the `volatility <= 0` guard was passed (NaN
    // comparisons are false) and Sharpe itself became NaN. That guard is still
    // what this pins: an empty result proves the branch was taken, because a
    // NaN volatility would have slipped past it and produced a value.
    //
    // The expected answer is no longer 0.0 either. A constant series has no
    // dispersion, so there is no Sharpe ratio to state.
    auto sharpe = calc_.calculate_sharpe_ratio({0.007, 0.007, 0.007, 0.007, 0.007}, 5, 0.0);
    EXPECT_FALSE(sharpe.has_value());
}

TEST_F(BacktestMetricsCalculatorTest, VolatilityComputesAnnualizedStdev) {
    std::vector<double> r{0.01, -0.01, 0.01, -0.01};
    // mean=0, var=0.0001, daily std = 0.01, annualized = 0.01 * sqrt(252)
    EXPECT_NEAR(calc_.calculate_volatility(r), 0.01 * std::sqrt(252.0), 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, DownsideVolatilityZeroWhenAllReturnsAboveTarget) {
    EXPECT_DOUBLE_EQ(calc_.calculate_downside_volatility({0.01, 0.02, 0.03}, 0.0), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, DownsideVolatilityCountsOnlyBelowTarget) {
    std::vector<double> r{-0.01, 0.01, -0.01, 0.01};
    // Two -0.01s contribute (0.0001 each); avg = 0.0001; sqrt = 0.01; * sqrt(252)
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 0.0),
                0.01 * std::sqrt(252.0), 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, SharpeWithNothingToMeasureIsUndefined) {
    EXPECT_FALSE(calc_.calculate_sharpe_ratio({}, 252).has_value());
}

TEST_F(BacktestMetricsCalculatorTest, SharpeWithNonPositiveTradingDaysIsUndefined) {
    EXPECT_FALSE(calc_.calculate_sharpe_ratio({0.01, 0.02}, 0).has_value());
    EXPECT_FALSE(calc_.calculate_sharpe_ratio({0.01, 0.02}, -1).has_value());
}

TEST_F(BacktestMetricsCalculatorTest, SharpeWithNoVolatilityIsUndefinedNotZero) {
    // Return per unit of risk, with no risk to divide by. Zero is not the
    // answer -- it reads as "earned nothing per unit of risk", which is a claim
    // about a quantity that has no value, and it averages into a fund-level
    // Sharpe exactly like a real one.
    std::vector<double> flat{0.01, 0.01, 0.01, 0.01};
    EXPECT_FALSE(calc_.calculate_sharpe_ratio(flat, 252).has_value());
    // And the reason is reported separately, as a real measurement.
    EXPECT_DOUBLE_EQ(calc_.calculate_volatility(flat), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SharpeIsMeasuredWhenVolatilityIsPositive) {
    std::vector<double> r{0.01, -0.01, 0.02, -0.01, 0.015, -0.005};
    auto sharpe = calc_.calculate_sharpe_ratio(r, 252);
    ASSERT_TRUE(sharpe.has_value());
    EXPECT_NE(*sharpe, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoWithNothingToMeasureIsUndefined) {
    EXPECT_FALSE(calc_.calculate_sortino_ratio({}, 252).has_value());
    EXPECT_FALSE(calc_.calculate_sortino_ratio({0.01}, 0).has_value());
}

TEST_F(BacktestMetricsCalculatorTest, SortinoWithNoDownsideIsUndefinedNotNineNineNine) {
    // Three up days and nothing below the target. The denominator is zero, so
    // there is no ratio -- this reported 999.0, which sorts to the top of any
    // leaderboard and averages into any fund-level figure it reaches.
    EXPECT_FALSE(calc_.calculate_sortino_ratio({0.01, 0.02, 0.03}, 252).has_value());
    // The reason is still available, and is not itself in doubt.
    EXPECT_DOUBLE_EQ(calc_.calculate_downside_volatility({0.01, 0.02, 0.03}, 0.0), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoWithNoDownsideIsUndefinedWhicheverWayTheMeanPoints) {
    // Returns all above target=-0.01, so no downside, but mean*252 is negative.
    // The old code answered 999.0 above and 0.0 here: two confident values for
    // the same division by zero, one reading as a triumph and one as a measured
    // absence of return.
    EXPECT_FALSE(calc_.calculate_sortino_ratio({-0.005, 0.001}, 252, -0.01).has_value());
}

TEST_F(BacktestMetricsCalculatorTest, SortinoComputesWithDownsideVolatility) {
    // Mean must be non-zero for the result to be non-zero.
    std::vector<double> r{0.02, -0.01, 0.02, -0.01};  // mean = 0.005
    auto sortino = calc_.calculate_sortino_ratio(r, 252);
    ASSERT_TRUE(sortino.has_value());
    EXPECT_GT(*sortino, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, CalmarWithNoDrawdownIsUndefined) {
    // An equity curve that never fell has no Calmar ratio. Both signs of the
    // numerator are the same division by zero.
    EXPECT_FALSE(calc_.calculate_calmar_ratio(0.10, 0.0).has_value());
    EXPECT_FALSE(calc_.calculate_calmar_ratio(-0.10, 0.0).has_value());
}

TEST_F(BacktestMetricsCalculatorTest, CalmarStandardCase) {
    auto calmar = calc_.calculate_calmar_ratio(0.20, 0.10);
    ASSERT_TRUE(calmar.has_value());
    EXPECT_DOUBLE_EQ(*calmar, 2.0);
}

TEST_F(BacktestMetricsCalculatorTest, DrawdownsEmptyCurveReturnsEmpty) {
    EXPECT_TRUE(calc_.calculate_drawdowns({}).empty());
    EXPECT_DOUBLE_EQ(calc_.calculate_max_drawdown({}), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, DrawdownsTrackPeakToTrough) {
    std::vector<std::pair<Timestamp, double>> curve;
    curve.emplace_back(date_at(2026, 1, 1), 100.0);
    curve.emplace_back(date_at(2026, 1, 2), 110.0);  // new peak
    curve.emplace_back(date_at(2026, 1, 3), 99.0);   // 10% drawdown from 110
    curve.emplace_back(date_at(2026, 1, 4), 105.0);  // partial recovery
    auto dd = calc_.calculate_drawdowns(curve);
    ASSERT_EQ(dd.size(), 4u);
    EXPECT_DOUBLE_EQ(dd[0].second, 0.0);
    EXPECT_DOUBLE_EQ(dd[1].second, 0.0);
    EXPECT_NEAR(dd[2].second, (110.0 - 99.0) / 110.0, 1e-9);
    EXPECT_NEAR(dd[3].second, (110.0 - 105.0) / 110.0, 1e-9);
    EXPECT_NEAR(calc_.calculate_max_drawdown(curve), (110.0 - 99.0) / 110.0, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, VarAndCvarEmptyReturnsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_var_95({}), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_cvar_95({}), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, Var95PicksWorst5thPercentileLoss) {
    std::vector<double> r;
    for (int i = -5; i <= 14; ++i) r.push_back(i * 0.01);
    // 20 returns. 5% index = 1; sorted[1] = -0.04; VaR = +0.04
    EXPECT_NEAR(calc_.calculate_var_95(r), 0.04, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, Cvar95AveragesTailLosses) {
    std::vector<double> r;
    for (int i = -10; i <= 9; ++i) r.push_back(i * 0.01);
    // 20 returns. var_index = 1; cvar averages first 1 = -0.10 → +0.10
    EXPECT_NEAR(calc_.calculate_cvar_95(r), 0.10, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, Cvar95FloorsVarIndexAtOneOnTinyInput) {
    // var_index = floor(3*0.05) = 0 → bumped to 1
    std::vector<double> r{-0.05, 0.0, 0.05};
    EXPECT_NEAR(calc_.calculate_cvar_95(r), 0.05, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, RiskMetricsEmptyReturnsEmpty) {
    EXPECT_TRUE(calc_.calculate_risk_metrics({}, 252).empty());
}

TEST_F(BacktestMetricsCalculatorTest, RiskMetricsAggregateContainsKeys) {
    auto m = calc_.calculate_risk_metrics({-0.05, 0.0, 0.05, -0.02, 0.03}, 252);
    EXPECT_TRUE(m.count("var_95"));
    EXPECT_TRUE(m.count("cvar_95"));
    EXPECT_TRUE(m.count("downside_volatility"));
}

TEST_F(BacktestMetricsCalculatorTest, BetaCorrelationTooFewReturnsIsZero) {
    auto p = calc_.calculate_beta_correlation({0.01});
    EXPECT_DOUBLE_EQ(p.first, 0.0);
    EXPECT_DOUBLE_EQ(p.second, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, BetaCorrelationConstantSeriesIsZero) {
    // All same → variance_benchmark = 0 → both returned as default 0
    auto p = calc_.calculate_beta_correlation({0.01, 0.01, 0.01, 0.01});
    EXPECT_DOUBLE_EQ(p.first, 0.0);
    EXPECT_DOUBLE_EQ(p.second, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, BetaCorrelationProducesFiniteForVaryingSeries) {
    auto p = calc_.calculate_beta_correlation({0.01, -0.01, 0.02, -0.005, 0.015, -0.01});
    EXPECT_TRUE(std::isfinite(p.first));
    EXPECT_TRUE(std::isfinite(p.second));
    EXPECT_GE(p.second, -1.0);
    EXPECT_LE(p.second, 1.0);
}

TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsEmptyExecutions) {
    auto s = calc_.calculate_trade_statistics({});
    EXPECT_EQ(s.total_trades, 0);
    EXPECT_EQ(s.winning_trades, 0);
    EXPECT_DOUBLE_EQ(s.win_rate, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsClosingTradeIsRecorded) {
    auto t0 = date_at(2026, 1, 1);
    std::vector<ExecutionReport> execs = {
        make_exec("ES", Side::BUY, 1.0, 100.0, t0, 1.0),  // open long
        make_exec("ES", Side::SELL, 1.0, 110.0, t0 + std::chrono::hours(48), 1.0),  // close
    };
    auto s = calc_.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1);
    EXPECT_EQ(s.winning_trades, 1);
    EXPECT_DOUBLE_EQ(s.win_rate, 1.0);
    EXPECT_DOUBLE_EQ(s.max_win, 9.0);  // 1*(110-100) - 1 commission = 9
    // No losing trade, so there is no denominator and no profit factor. This
    // used to assert 999.0 -- the test agreed with the sentinel rather than
    // asking whether a sentinel belonged in a numeric field at all.
    EXPECT_FALSE(s.profit_factor.has_value());
    EXPECT_NEAR(s.avg_holding_period, 2.0, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsLosingTradeRecorded) {
    auto t0 = date_at(2026, 1, 1);
    std::vector<ExecutionReport> execs = {
        make_exec("ES", Side::BUY, 1.0, 110.0, t0, 1.0),
        make_exec("ES", Side::SELL, 1.0, 100.0, t0 + std::chrono::hours(24), 1.0),  // -10 - 1
    };
    auto s = calc_.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1);
    EXPECT_EQ(s.winning_trades, 0);
    EXPECT_DOUBLE_EQ(s.win_rate, 0.0);
    EXPECT_DOUBLE_EQ(s.max_loss, 11.0);
    // Losses but no profit: the ratio is a real zero, not an absence.
    ASSERT_TRUE(s.profit_factor.has_value());
    EXPECT_DOUBLE_EQ(*s.profit_factor, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsAddingThenClosingComputesAvgEntry) {
    auto t0 = date_at(2026, 1, 1);
    std::vector<ExecutionReport> execs = {
        make_exec("ES", Side::BUY, 2.0, 100.0, t0),
        make_exec("ES", Side::BUY, 2.0, 110.0, t0 + std::chrono::hours(24)),  // avg → 105
        make_exec("ES", Side::SELL, 4.0, 120.0, t0 + std::chrono::hours(48)), // close at 120
    };
    auto s = calc_.calculate_trade_statistics(execs);
    EXPECT_EQ(s.total_trades, 1);
    EXPECT_DOUBLE_EQ(s.total_profit, 4.0 * (120.0 - 105.0));
}

TEST_F(BacktestMetricsCalculatorTest, SymbolPnLAccumulatesPerSymbol) {
    auto t0 = date_at(2026, 1, 1);
    std::vector<ExecutionReport> execs = {
        make_exec("ES", Side::BUY, 1.0, 100.0, t0),
        make_exec("ES", Side::SELL, 1.0, 110.0, t0 + std::chrono::hours(24)),
        make_exec("NQ", Side::BUY, 1.0, 200.0, t0),
        make_exec("NQ", Side::SELL, 1.0, 195.0, t0 + std::chrono::hours(24)),
    };
    auto m = calc_.calculate_symbol_pnl(execs);
    EXPECT_DOUBLE_EQ(m["ES"], 10.0);
    EXPECT_DOUBLE_EQ(m["NQ"], -5.0);
}

TEST_F(BacktestMetricsCalculatorTest, MonthlyReturnsAggregatesByYearMonthKey) {
    // Use mid-month dates so local-time conversion doesn't shift days across months.
    std::vector<std::pair<Timestamp, double>> curve;
    curve.emplace_back(date_at(2026, 1, 15), 100.0);
    curve.emplace_back(date_at(2026, 1, 20), 110.0);  // Jan: +10%
    curve.emplace_back(date_at(2026, 2, 15), 121.0);  // Feb: +10%
    auto m = calc_.calculate_monthly_returns(curve);
    EXPECT_NEAR(m["2026-01"], 0.10, 1e-9);
    EXPECT_NEAR(m["2026-02"], 0.10, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, MonthlyReturnsSkipsNonPositivePriorEquity) {
    // Regression: dividing by a zero prior equity injected inf into the
    // monthly totals and every later sum on them.
    std::vector<std::pair<Timestamp, double>> curve;
    curve.emplace_back(date_at(2026, 1, 15), 100.0);
    curve.emplace_back(date_at(2026, 1, 16), 0.0);    // wiped out
    curve.emplace_back(date_at(2026, 1, 20), 50.0);   // prior equity 0: skipped
    curve.emplace_back(date_at(2026, 1, 22), 55.0);   // +10%
    auto m = calc_.calculate_monthly_returns(curve);
    EXPECT_TRUE(std::isfinite(m["2026-01"]));
    // -100% (100 -> 0) plus +10% (50 -> 55); the 0 -> 50 step is skipped.
    EXPECT_NEAR(m["2026-01"], -1.0 + 0.10, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, FilterWarmupTrimsLeadingDays) {
    auto curve = linear_equity_curve(10, 100.0, 1.0);
    // Calculator's filter is private but exercised via calculate_all_metrics:
    backtest::BacktestResults r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/3);
    EXPECT_GT(r.total_return, 0.0);
    // total_return ~= (109-103)/103
    EXPECT_NEAR(r.total_return, (109.0 - 103.0) / 103.0, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsEmptyCurveReturnsDefaults) {
    auto r = calc_.calculate_all_metrics({}, {}, 0);
    EXPECT_DOUBLE_EQ(r.total_return, 0.0);
    EXPECT_DOUBLE_EQ(r.max_drawdown, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsWarmupGreaterThanSizePassesFullCurveThrough) {
    // filter_warmup_period treats warmup>=size the same as warmup<=0: it returns
    // the unfiltered curve so the metrics still compute on the full series.
    auto curve = linear_equity_curve(3, 100.0, 1.0);
    auto r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/100);
    EXPECT_NEAR(r.total_return, (102.0 - 100.0) / 100.0, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsPopulatesNonZeroFields) {
    std::vector<std::pair<Timestamp, double>> curve;
    for (int i = 0; i < 30; ++i) {
        double v = 100.0 + (i % 2 == 0 ? i : -i) * 0.5;
        curve.emplace_back(date_at(2026, 1, 1) + std::chrono::hours(24 * i), v);
    }
    auto r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/0);
    EXPECT_NE(r.volatility, 0.0);
    EXPECT_FALSE(r.drawdown_curve.empty());
    EXPECT_GE(r.max_drawdown, 0.0);
}

// ===== NaN / degeneracy guards (batch-2 metrics hardening) =====

TEST_F(BacktestMetricsCalculatorTest, NanEquityPointExcludedFromMonthlyReturns) {
    // NaN in the middle of the curve: neither the NaN-as-current nor the
    // NaN-as-previous transition may contribute. Pre-fix, `prev <= 0.0` was
    // false for NaN and a NaN period_return poisoned the month's total.
    std::vector<std::pair<Timestamp, double>> curve = {
        {date_at(2024, 3, 1), 100.0},
        {date_at(2024, 3, 4), 110.0},
        {date_at(2024, 3, 5), std::numeric_limits<double>::quiet_NaN()},
        {date_at(2024, 3, 6), 120.0},
        {date_at(2024, 3, 7), 126.0}};
    auto monthly = calc_.calculate_monthly_returns(curve);
    ASSERT_EQ(monthly.size(), 1u);
    const double total = monthly.at("2024-03");
    EXPECT_TRUE(std::isfinite(total));
    // Surviving transitions: 100->110 (+10%) and 120->126 (+5%).
    EXPECT_NEAR(total, 0.10 + 0.05, 1e-12);
}

TEST_F(BacktestMetricsCalculatorTest, NanEquityPointExcludedFromReturnSeries) {
    std::vector<std::pair<Timestamp, double>> curve = {
        {date_at(2024, 3, 1), 100.0},
        {date_at(2024, 3, 4), std::numeric_limits<double>::quiet_NaN()},
        {date_at(2024, 3, 5), 100.0},
        {date_at(2024, 3, 6), 105.0}};
    auto returns = calc_.calculate_returns_from_equity(curve);
    ASSERT_EQ(returns.size(), 1u);
    EXPECT_NEAR(returns[0], 0.05, 1e-12);
}

TEST_F(BacktestMetricsCalculatorTest, TotalReturnNanInputsReturnZero) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_DOUBLE_EQ(calc_.calculate_total_return(nan, 110.0), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_total_return(100.0, nan), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoNearTargetDustIsUndefinedNotAnAbsurdValue) {
    // Returns an epsilon below target leave ~1e-17 downside "dust". Sortino
    // divided by the dust and reported an absurd finite magnitude; the dust now
    // collapses to 0, and a zero denominator has no ratio at all. This test
    // used to settle for "at most 999", which was only ever a bound on how
    // wrong the answer could be.
    std::vector<double> returns(100, -1e-18);
    EXPECT_FALSE(
        calc_.calculate_sortino_ratio(returns, /*trading_days=*/100, /*mar=*/0.0).has_value());
    const double downside = calc_.calculate_downside_volatility(returns, 0.0);
    EXPECT_DOUBLE_EQ(downside, 0.0);
}
