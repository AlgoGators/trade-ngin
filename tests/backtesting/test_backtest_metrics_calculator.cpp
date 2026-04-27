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

TEST_F(BacktestMetricsCalculatorTest, SharpeEmptyReturnsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_sharpe_ratio({}, 252), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SharpeNonPositiveDaysReturnsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_sharpe_ratio({0.01, 0.02}, 0), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_sharpe_ratio({0.01, 0.02}, -1), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SharpeNonZeroWhenVolatilityPositive) {
    std::vector<double> r{0.01, -0.01, 0.02, -0.01, 0.015, -0.005};
    EXPECT_NE(calc_.calculate_sharpe_ratio(r, 252), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoEmptyReturnsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_sortino_ratio({}, 252), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_sortino_ratio({0.01}, 0), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoCappedAt999WhenNoDownsideAndPositive) {
    EXPECT_DOUBLE_EQ(calc_.calculate_sortino_ratio({0.01, 0.02, 0.03}, 252), 999.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoZeroWhenNoDownsideAndNegativeAnnualMean) {
    // Returns all above target=-0.01 (no downside) but mean*252 is negative.
    EXPECT_DOUBLE_EQ(calc_.calculate_sortino_ratio({-0.005, 0.001}, 252, -0.01), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoComputesWithDownsideVolatility) {
    // Mean must be non-zero for the result to be non-zero.
    std::vector<double> r{0.02, -0.01, 0.02, -0.01};  // mean = 0.005
    EXPECT_GT(calc_.calculate_sortino_ratio(r, 252), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, CalmarHandlesZeroDrawdown) {
    EXPECT_DOUBLE_EQ(calc_.calculate_calmar_ratio(0.10, 0.0), 999.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_calmar_ratio(-0.10, 0.0), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, CalmarStandardCase) {
    EXPECT_DOUBLE_EQ(calc_.calculate_calmar_ratio(0.20, 0.10), 2.0);
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
    EXPECT_DOUBLE_EQ(s.profit_factor, 999.0);  // no losses, has wins
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
    EXPECT_DOUBLE_EQ(s.profit_factor, 0.0);  // no profit
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
