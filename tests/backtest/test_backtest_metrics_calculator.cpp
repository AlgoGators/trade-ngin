#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/transaction_cost/netting.hpp"

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

// One dollar a point: the tests whose numbers are price points.
const BacktestMetricsCalculator::PointValueSource kUnitPoints = [](const std::string&) { return 1.0; };

BacktestMetricsCalculator::PointValueSource points_of(std::unordered_map<std::string, double> table) {
    return [table = std::move(table)](const std::string& symbol) { return table.at(symbol); };
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

TEST_F(BacktestMetricsCalculatorTest, SharpeConstantSeriesIsZeroNotNaN) {
    // With NaN volatility the `volatility <= 0` guard was passed (NaN
    // comparisons are false) and Sharpe itself became NaN.
    auto sharpe = calc_.calculate_sharpe_ratio({0.007, 0.007, 0.007, 0.007, 0.007}, 5, 0.0);
    EXPECT_TRUE(std::isfinite(sharpe));
    EXPECT_DOUBLE_EQ(sharpe, 0.0);
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
    // Two -0.01s contribute (0.0001 each), summed 0.0002 and averaged over ALL FOUR returns:
    // 0.00005; sqrt = 0.0070710678; * sqrt(252) = 0.112249722
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 0.0), 0.112249722, 1e-9);
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 0.0),
                std::sqrt(0.0002 / 4.0) * std::sqrt(252.0), 1e-12);
}

TEST_F(BacktestMetricsCalculatorTest, DownsideVolatilityAndSortinoAverageOverEveryReturn) {
    std::vector<double> r{0.01, -0.02, 0.03, -0.01};
    // Below target 0: -0.02 and -0.01, squares 0.0004 + 0.0001 = 0.0005, over 4 returns =
    // 0.000125; sqrt = 0.0111803399; * sqrt(252) = 0.177482393.
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 0.0), 0.177482393, 1e-9);
    // mean = 0.0025, * 252 = 0.63; 0.63 / 0.177482393 = 3.549647870.
    EXPECT_NEAR(calc_.calculate_sortino_ratio(r, 4), 3.549647870, 1e-8);
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
    auto s = calc_.calculate_trade_statistics({}, kUnitPoints);
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
    auto s = calc_.calculate_trade_statistics(execs, kUnitPoints);
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
    auto s = calc_.calculate_trade_statistics(execs, kUnitPoints);
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
    auto s = calc_.calculate_trade_statistics(execs, kUnitPoints);
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
    auto m = calc_.calculate_symbol_pnl(execs, kUnitPoints);
    EXPECT_DOUBLE_EQ(m["ES"], 10.0);
    EXPECT_DOUBLE_EQ(m["NQ"], -5.0);
}

// T-8D R11 (a), S3b 7.1: a trade is scored in DOLLARS, the price move times the symbol's point
// value (the metadata's contract size, the figure the equity curve books with). Real fills of the
// futures book (MES n=70 and ZN n=128 of the run S3b read):
//
//   | row            | closed | entry       | exit       | points    | point value | gross     | cost        | trade        |
//   | MES SELL 1     | 1      | 6785.25     | 6641.75    | -143.5    | 5           | -717.50   | 2.14618282  | -719.646183  |
//   | ZN  SELL 2     | 2      | 113.2578125 | 112.390625 | -0.8671875| 1000        | -1734.375 | 11.00661929 | -1745.381619 |
//
// RED on the parent: the price term is in points (MES -145.646183, ZN -12.740994).
TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsPricesFuturesTradesInDollars) {
    auto t0 = date_at(2025, 11, 10);
    auto day = [&](int k) { return t0 + std::chrono::hours(24 * k); };
    std::vector<ExecutionReport> execs = {
        make_exec("MES.v.0", Side::BUY, 1.0, 6785.25, day(0), 2.14870078),
        make_exec("ZN.v.0", Side::BUY, 1.0, 113.1875, day(0), 5.47021144),
        make_exec("MES.v.0", Side::SELL, 1.0, 6641.75, day(9), 2.14618282),
        make_exec("ZN.v.0", Side::BUY, 1.0, 113.328125, day(9), 5.49488994),
        make_exec("ZN.v.0", Side::SELL, 2.0, 112.390625, day(19), 11.00661929),
    };
    const auto point_value = points_of({{"MES.v.0", 5.0}, {"ZN.v.0", 1000.0}});
    auto s = calc_.calculate_trade_statistics(execs, point_value);
    EXPECT_EQ(s.total_trades, 2);
    EXPECT_EQ(s.winning_trades, 0);
    EXPECT_NEAR(s.max_loss, 1745.381619, 1e-6);
    EXPECT_NEAR(s.total_loss, 719.646183 + 1745.381619, 1e-6);
    EXPECT_NEAR(s.avg_loss, (719.646183 + 1745.381619) / 2, 1e-6);
    // The symbol's P&L is the same dollars, with the opening fills' costs charged too.
    auto m = calc_.calculate_symbol_pnl(execs, point_value);
    EXPECT_NEAR(m["MES.v.0"], -719.646183 - 2.14870078, 1e-6);
    EXPECT_NEAR(m["ZN.v.0"], -1745.381619 - 5.47021144 - 5.49488994, 1e-6);
}

// T-8D R11, S3b 7.2: a fill that takes the position through zero closes what was held and opens the
// remainder at ITS price.
//
//   | day | fill          | position | entry | trade                              |
//   | 0   | BUY 2 at 100  | +2       | 100   |                                    |
//   | 1   | SELL 3 at 110 | -1       | 110   | closes 2: 2 x (110 - 100) = +20    |
//   | 2   | BUY 1 at 105  | 0        |       | closes the short: 110 - 105 = +5   |
//
// RED on the parent: the short keeps the long's entry 100 and scores 100 - 105 = -5.
TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsFlipOpensTheRemainderAtTheFillPrice) {
    auto t0 = date_at(2026, 1, 1);
    auto day = [&](int k) { return t0 + std::chrono::hours(24 * k); };
    std::vector<ExecutionReport> execs = {
        make_exec("X", Side::BUY, 2.0, 100.0, day(0)),
        make_exec("X", Side::SELL, 3.0, 110.0, day(1)),
        make_exec("X", Side::BUY, 1.0, 105.0, day(2)),
    };
    auto s = calc_.calculate_trade_statistics(execs, kUnitPoints);
    EXPECT_EQ(s.total_trades, 2);
    EXPECT_EQ(s.winning_trades, 2);
    EXPECT_DOUBLE_EQ(s.total_profit, 25.0);
    EXPECT_DOUBLE_EQ(s.total_loss, 0.0);
    EXPECT_DOUBLE_EQ(s.max_loss, 0.0);
    auto m = calc_.calculate_symbol_pnl(execs, kUnitPoints);
    EXPECT_DOUBLE_EQ(m["X"], 25.0);  // the fills' cash: -200 + 330 - 105
}

// T-8D R11 (d), S3b 7.3 (E2-F60): a position under 1e-9 after a fill is flat. Fractional share
// quantities summed in binary leave a residue after a full close (0.3 - 0.1 - 0.2 = -2.8e-17):
//
//   | day | fill            | position            | trade                                  |
//   | 0   | BUY 0.3 at 100  | 0.3                 |                                        |
//   | 1   | SELL 0.1 at 110 | 0.2                 | 0.1 x 10 = +1                          |
//   | 2   | SELL 0.2 at 110 | flat (-2.8e-17)     | 0.2 x 10 = +2                          |
//   | 5   | BUY 1 at 200    | +1 at 200           | none: it opens                         |
//   | 6   | SELL 1 at 210   | 0                   | 210 - 200 = +10                        |
//
// RED on the parent: the residue is a short position, the BUY of day 5 "closes" it (a fourth
// trade, a loss of 2.8e-15) and the last SELL is measured from 100 (+110).
TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsFloatResidueIsFlat) {
    auto t0 = date_at(2026, 1, 1);
    auto day = [&](int k) { return t0 + std::chrono::hours(24 * k); };
    std::vector<ExecutionReport> execs = {
        make_exec("X", Side::BUY, 0.3, 100.0, day(0)),
        make_exec("X", Side::SELL, 0.1, 110.0, day(1)),
        make_exec("X", Side::SELL, 0.2, 110.0, day(2)),
        make_exec("X", Side::BUY, 1.0, 200.0, day(5)),
        make_exec("X", Side::SELL, 1.0, 210.0, day(6)),
    };
    auto s = calc_.calculate_trade_statistics(execs, kUnitPoints);
    EXPECT_EQ(s.total_trades, 3);
    EXPECT_NEAR(s.max_win, 10.0, 1e-9);
    EXPECT_NEAR(s.total_profit, 13.0, 1e-9);
    EXPECT_DOUBLE_EQ(s.total_loss, 0.0);
    EXPECT_DOUBLE_EQ(s.profit_factor, 999.0);
    EXPECT_NEAR(calc_.calculate_symbol_pnl(execs, kUnitPoints)["X"], 13.0, 1e-9);
}

namespace {

// Real rows of the BASE backtest (two sleeves; lookback 2 to 2026-10-07 at d440f84a; the
// backtest.executions ids are in the last column), three contracts. The statistics are the
// BOOK's (HD 2026-10-10): the STRATEGY rows of one contract on one day are first netted into the
// account's fill. T = TREND_FOLLOWING, F = TREND_FOLLOWING_FAST; cost = own cost - adjustment.
//
// ZF (point value 1000):
//   | date       | sleeve rows (side qty, net cost)                         | account fill      | cost        | position (entry)    | trade, dollars                                    |
//   | 2024-08-05 | F BUY 1 (3.55381234), T BUY 1 (3.55381235)                | BUY 2 at 109.59375| 7.10762469  | +2 (109.59375)      | same side: ONE fill                               |
//   | 2024-08-18 | F SELL 1 (3.57076615)                                     | SELL 1 at 108.796875 | 3.57076615 | +1               | 1 x (108.796875 - 109.59375) x 1000 - 3.57076615 = -800.44576615 |
//   | 2024-08-19 | T SELL 1 (3.56994255)                                     | SELL 1 at 108.6640625 | 3.56994255 | 0               | -929.6875 - 3.56994255 = -933.25744255            |
//   | 2024-09-08 | T BUY 1 (3.53728533)                                      | BUY 1 at 110.40625 | 3.53728533 | +1                  |                                                   |
//   | 2024-09-11 | F BUY 1 (3.56532536)                                      | BUY 1 at 110.7421875 | 3.56532536 | +2               |                                                   |
//   | 2024-09-16 | T BUY 1 (3.57076757)                                      | BUY 1 at 110.65625 | 3.57076757 | +3                  |                                                   |
//   | 2024-09-17 | F BUY 1 (3.70565228)                                      | BUY 1 at 110.71875 | 3.70565228 | +4 (110.630859375)  |                                                   |
//   | 2024-09-19 | F SELL 1 (0), T BUY 1 (0)                                 | none              |             | +4                  | a FULL CROSS: no fill, nothing scored             |
//   | 2024-09-27 | F SELL 1 (0), T BUY 1 (0)                                 | none              |             | +4                  | full cross                                        |
//   | 2024-09-29 | F BUY 1 (0), T SELL 1 (0)                                 | none              |             | +4                  | full cross                                        |
//   | 2024-10-03 | F SELL 1 (0), T BUY 1 (0)                                 | none              |             | +4                  | full cross                                        |
//   | 2024-10-04 | F SELL 1 (0), T SELL 4 (14.65250399), T BUY 1 (0)         | SELL 4 at 109.6484375 | 14.65250399 | 0              | a PARTIAL OFFSET, one fill of the net size: 4 x (109.6484375 - 110.630859375) x 1000 - 14.65250399 = -3944.34000399 |
//   ids: 4, 1107; 8; 1124; 1156; 28; 1164; 36; 39, 1179; 55, 1200; 57, 1203; 62, 1208; 64, 1210, 1211.
//
// 6C (point value 100000):
//   | 2024-08-05 | T SELL 1 (4.55172564)                                     | SELL 1 at 0.72125 | 4.55172564  | -1                  |                                                   |
//   | 2024-08-14 | T BUY 1 (4.93313484)                                      | BUY 1 at 0.7302   | 4.93313484  | 0                   | (0.72125 - 0.7302) x 100000 - 4.93313484 = -899.93313484 |
//   | 2024-08-19 | F BUY 1 (4.85984294)                                      | BUY 1 at 0.7317   | 4.85984294  | +1                  |                                                   |
//   | 2024-08-25 | F SELL 1 (0), T BUY 1 (0)                                 | none              |             | +1                  | full cross: the account still holds +1            |
//   ids: 1099; 1112; 9; 12, 1129.
//
// MES (point value 5):
//   | 2024-08-15 | T BUY 1 (1.31259197)                                      | BUY 1 at 5474.75  | 1.31259197  | +1                  |                                                   |
//   | 2024-09-06 | T SELL 1 (1.26034345)                                     | SELL 1 at 5510    | 1.26034345  | 0                   | 35.25 x 5 - 1.26034345 = +174.98965655            |
//   ids: 1114; 1149.
//
// 26 sleeve rows, 13 account fills, 5 trades, 1 win.
std::vector<ExecutionReport> base_rows() {
    auto row = [](const std::string& symbol, Side side, double qty, double price, Timestamp ts,
                  double own, double adjustment) {
        ExecutionReport e = make_exec(symbol, side, qty, price, ts, own);
        e.netting_adjustment = Decimal(adjustment);
        return e;
    };
    const std::string zf = "ZF.v.0", c6 = "6C.v.0", mes = "MES.v.0";
    return {
        row(zf, Side::BUY, 1, 109.59375, date_at(2024, 8, 5), 3.52725064, -0.0265617),
        row(zf, Side::BUY, 1, 109.59375, date_at(2024, 8, 5), 3.52725064, -0.02656171),
        row(zf, Side::SELL, 1, 108.796875, date_at(2024, 8, 18), 3.57076615, 0),
        row(zf, Side::SELL, 1, 108.6640625, date_at(2024, 8, 19), 3.56994255, 0),
        row(zf, Side::BUY, 1, 110.40625, date_at(2024, 9, 8), 3.53728533, 0),
        row(zf, Side::BUY, 1, 110.7421875, date_at(2024, 9, 11), 3.56532536, 0),
        row(zf, Side::BUY, 1, 110.65625, date_at(2024, 9, 16), 3.57076757, 0),
        row(zf, Side::BUY, 1, 110.71875, date_at(2024, 9, 17), 3.70565228, 0),
        row(zf, Side::SELL, 1, 110.2890625, date_at(2024, 9, 19), 3.55618504, 3.55618504),
        row(zf, Side::BUY, 1, 110.2890625, date_at(2024, 9, 19), 3.55618504, 3.55618504),
        row(zf, Side::SELL, 1, 109.9765625, date_at(2024, 9, 27), 3.56623753, 3.56623753),
        row(zf, Side::BUY, 1, 109.9765625, date_at(2024, 9, 27), 3.56623753, 3.56623753),
        row(zf, Side::BUY, 1, 110.2421875, date_at(2024, 9, 29), 3.56426973, 3.56426973),
        row(zf, Side::SELL, 1, 110.2421875, date_at(2024, 9, 29), 3.56426973, 3.56426973),
        row(zf, Side::SELL, 1, 109.9453125, date_at(2024, 10, 3), 3.68571236, 3.68571236),
        row(zf, Side::BUY, 1, 109.9453125, date_at(2024, 10, 3), 3.68571236, 3.68571236),
        row(zf, Side::SELL, 1, 109.6484375, date_at(2024, 10, 4), 3.5631255, 3.5631255),
        row(zf, Side::SELL, 4, 109.6484375, date_at(2024, 10, 4), 14.65250399, 0),
        row(zf, Side::BUY, 1, 109.6484375, date_at(2024, 10, 4), 3.5631255, 3.5631255),
        row(c6, Side::SELL, 1, 0.72125, date_at(2024, 8, 5), 4.55172564, 0),
        row(c6, Side::BUY, 1, 0.7302, date_at(2024, 8, 14), 4.93313484, 0),
        row(c6, Side::BUY, 1, 0.7317, date_at(2024, 8, 19), 4.85984294, 0),
        row(c6, Side::SELL, 1, 0.7408, date_at(2024, 8, 25), 4.63555201, 4.63555201),
        row(c6, Side::BUY, 1, 0.7408, date_at(2024, 8, 25), 4.63555201, 4.63555201),
        row(mes, Side::BUY, 1, 5474.75, date_at(2024, 8, 15), 1.31259197, 0),
        row(mes, Side::SELL, 1, 5510.0, date_at(2024, 9, 6), 1.26034345, 0),
    };
}

BacktestMetricsCalculator::PointValueSource base_points() {
    return points_of({{"ZF.v.0", 1000.0}, {"6C.v.0", 100000.0}, {"MES.v.0", 5.0}});
}

constexpr double kBaseLoss = 800.44576615 + 933.25744255 + 3944.34000399 + 899.93313484;
constexpr double kBaseWin = 174.98965655;

}  // namespace

// LEAD_RULINGS_IN_SESSION R-G (HD 2026-10-10): the trade statistics are the book's. The table
// above: a full cross is no fill and no trade; a partial offset is one fill of the net size,
// charged the sum of its rows' costs after netting; two sleeves on one side are one fill.
// RED on the parent: every sleeve row is a fill of one tracker, in points: the crossed rows score
// trades (the 6C cross a "win" of 0.00455 or 0.0091 by the order of the sleeves) and the dollars
// are price points.
TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsAreTheBooksAndACrossIsNeitherAFillNorATrade) {
    const auto execs = base_rows();
    ASSERT_EQ(execs.size(), 26u);
    auto s = calc_.calculate_trade_statistics(execs, base_points());
    EXPECT_EQ(s.strategy_fills, 13) << "the account's fills: 8 of ZF, 3 of 6C, 2 of MES";
    EXPECT_EQ(s.total_trades, 5);
    EXPECT_EQ(s.winning_trades, 1) << "no crossed row is a win";
    EXPECT_NEAR(s.win_rate, 0.2, 1e-12);
    EXPECT_NEAR(s.total_profit, kBaseWin, 1e-7);
    EXPECT_NEAR(s.avg_win, kBaseWin, 1e-7);
    EXPECT_NEAR(s.max_win, kBaseWin, 1e-7);
    EXPECT_NEAR(s.total_loss, kBaseLoss, 1e-7);
    EXPECT_NEAR(s.avg_loss, kBaseLoss / 4.0, 1e-7);
    EXPECT_NEAR(s.max_loss, 3944.34000399, 1e-7) << "the partial offset: SELL 4, the net of three rows";
    EXPECT_NEAR(s.profit_factor, kBaseWin / kBaseLoss, 1e-9);
    EXPECT_NEAR(s.avg_holding_period, (13.0 + 1.0 + 26.0 + 9.0 + 22.0) / 5.0, 1e-9);

    // The account's fills themselves.
    const auto fills = BacktestMetricsCalculator::account_fills(execs);
    ASSERT_EQ(fills.size(), 13u);
    const ExecutionReport* same_side = nullptr;
    const ExecutionReport* offset = nullptr;
    for (const auto& f : fills) {
        EXPECT_NE(f.fill_time, date_at(2024, 9, 19)) << "a full cross is no fill";
        EXPECT_NE(f.fill_time, date_at(2024, 8, 25)) << "a full cross is no fill";
        if (f.symbol == "ZF.v.0" && f.fill_time == date_at(2024, 8, 5)) same_side = &f;
        if (f.symbol == "ZF.v.0" && f.fill_time == date_at(2024, 10, 4)) offset = &f;
    }
    ASSERT_NE(same_side, nullptr);
    EXPECT_EQ(same_side->side, Side::BUY);
    EXPECT_EQ(same_side->filled_quantity, Quantity(2.0));
    EXPECT_EQ(transaction_cost::net_cost(*same_side), Decimal(7.10762469))
        << "the two rows' costs after netting: a NEGATIVE adjustment raises the charge";
    ASSERT_NE(offset, nullptr);
    EXPECT_EQ(offset->side, Side::SELL);
    EXPECT_EQ(offset->filled_quantity, Quantity(4.0)) << "SELL 1 + SELL 4 + BUY 1";
    EXPECT_EQ(offset->fill_price, Price(109.6484375));
    EXPECT_EQ(transaction_cost::net_cost(*offset), Decimal(14.65250399));

    // The symbols' P&L: the same fills, every one's cost charged.
    auto m = calc_.calculate_symbol_pnl(execs, base_points());
    EXPECT_NEAR(m["ZF.v.0"], -796.875 - 929.6875 - 3929.6875 - 43.27986792, 1e-7);
    EXPECT_NEAR(m["6C.v.0"], -895.0 - 4.55172564 - 4.93313484 - 4.85984294, 1e-7);
    EXPECT_NEAR(m["MES.v.0"], 176.25 - 1.31259197 - 1.26034345, 1e-7);
}

// R-G point 4: the order is the calculator's own (by day, then contract), so the order the rows
// arrive in changes nothing. The same rows reversed, and in seven seeded shuffles, give the same
// statistics to the last bit and the same account fills.
// RED on the parent: its one walk follows the input order (reversed, the first row "closes").
TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsDoNotDependOnTheOrderOfTheRows) {
    const auto rows = base_rows();
    const auto want = calc_.calculate_trade_statistics(rows, base_points());
    const auto want_pnl = calc_.calculate_symbol_pnl(rows, base_points());
    ASSERT_EQ(want.total_trades, 5);
    ASSERT_NEAR(want.max_loss, 3944.34000399, 1e-7);
    std::vector<std::vector<ExecutionReport>> orders;
    orders.emplace_back(rows.rbegin(), rows.rend());
    uint64_t state = 20261010;
    for (int k = 0; k < 7; ++k) {
        auto v = rows;
        for (size_t i = v.size() - 1; i > 0; --i) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            std::swap(v[i], v[(state >> 33) % (i + 1)]);
        }
        orders.push_back(std::move(v));
    }
    for (size_t i = 0; i < orders.size(); ++i) {
        const auto got = calc_.calculate_trade_statistics(orders[i], base_points());
        EXPECT_EQ(got.strategy_fills, want.strategy_fills) << "order " << i;
        EXPECT_EQ(got.total_trades, want.total_trades) << "order " << i;
        EXPECT_EQ(got.win_rate, want.win_rate) << "order " << i;
        EXPECT_EQ(got.profit_factor, want.profit_factor) << "order " << i;
        EXPECT_EQ(got.avg_win, want.avg_win) << "order " << i;
        EXPECT_EQ(got.avg_loss, want.avg_loss) << "order " << i;
        EXPECT_EQ(got.max_win, want.max_win) << "order " << i;
        EXPECT_EQ(got.max_loss, want.max_loss) << "order " << i;
        EXPECT_EQ(got.avg_holding_period, want.avg_holding_period) << "order " << i;
        EXPECT_EQ(calc_.calculate_symbol_pnl(orders[i], base_points()), want_pnl) << "order " << i;
    }
}

// A sleeve that reverses its position stores the fill as two rows on one side (the close and the
// open); they are the account's one order. Real rows of the CONSERVATIVE backtest (one sleeve),
// MBT on 2024-09-19 (ids 72, 73), the account short 2 from 54372.5 (point value 0.1):
//
//   | rows                                               | account fill      | cost       | trade                                                    |
//   | BUY 2 at 61900 (4.97008617), BUY 1 (2.44138938)    | BUY 3 at 61900    | 7.41147555 | 2 x (54372.5 - 61900) x 0.1 - 7.41147555 = -1512.91147555 |
//
// and the remainder, long 1, is opened at 61900.
TEST_F(BacktestMetricsCalculatorTest, TradeStatisticsOneSleevesTwoRowsOfADayAreOneAccountFill) {
    std::vector<ExecutionReport> execs = {
        make_exec("MBT.v.0", Side::SELL, 2.0, 54372.5, date_at(2024, 9, 1), 5.0),
        make_exec("MBT.v.0", Side::BUY, 2.0, 61900.0, date_at(2024, 9, 19), 4.97008617),
        make_exec("MBT.v.0", Side::BUY, 1.0, 61900.0, date_at(2024, 9, 19), 2.44138938),
        make_exec("MBT.v.0", Side::SELL, 1.0, 62000.0, date_at(2024, 9, 25), 2.5),
    };
    auto s = calc_.calculate_trade_statistics(execs, points_of({{"MBT.v.0", 0.1}}));
    EXPECT_EQ(s.strategy_fills, 3);
    EXPECT_EQ(s.total_trades, 2);
    EXPECT_NEAR(s.max_loss, 1512.91147555, 1e-7);
    EXPECT_NEAR(s.total_profit, 100.0 * 0.1 - 2.5, 1e-9) << "the long 1 was opened at 61900";
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
    backtest::BacktestResults r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/3, kUnitPoints);
    EXPECT_GT(r.total_return, 0.0);
    // total_return ~= (109-103)/103
    EXPECT_NEAR(r.total_return, (109.0 - 103.0) / 103.0, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsEmptyCurveReturnsDefaults) {
    auto r = calc_.calculate_all_metrics({}, {}, 0, kUnitPoints);
    EXPECT_DOUBLE_EQ(r.total_return, 0.0);
    EXPECT_DOUBLE_EQ(r.max_drawdown, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsWarmupGreaterThanSizePassesFullCurveThrough) {
    // filter_warmup_period treats warmup>=size the same as warmup<=0: it returns
    // the unfiltered curve so the metrics still compute on the full series.
    auto curve = linear_equity_curve(3, 100.0, 1.0);
    auto r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/100, kUnitPoints);
    EXPECT_NEAR(r.total_return, (102.0 - 100.0) / 100.0, 1e-9);
}

// A UTC date the curve carries twice counts once, by its LAST row: the metrics of the repeated
// curve equal the metrics of the curve with one row per date. Before the de-duplication the
// repeated date added a return of its own (two more observations here, one of them a 20 percent
// fall that no date-level series holds).
TEST_F(BacktestMetricsCalculatorTest, AllMetricsCountAUtcDateOnceByItsLastRow) {
    const std::vector<double> levels = {100.0, 102.0, 101.0, 104.0, 103.0, 106.0};
    std::vector<std::pair<Timestamp, double>> one_per_date;
    for (size_t i = 0; i < levels.size(); ++i) {
        one_per_date.emplace_back(
            date_at(2026, 1, 1) + std::chrono::hours(24 * static_cast<int>(i)) +
                std::chrono::hours(10),
            levels[i]);
    }
    // 2026-01-03 (index 2) and 2026-01-05 (index 4) each get an earlier row of the same UTC
    // date at 04:00, with a level the date did not close on.
    std::vector<std::pair<Timestamp, double>> repeated;
    for (size_t i = 0; i < one_per_date.size(); ++i) {
        if (i == 2) {
            repeated.emplace_back(one_per_date[i].first - std::chrono::hours(6), 80.0);
        }
        if (i == 4) {
            repeated.emplace_back(one_per_date[i].first - std::chrono::hours(6), 110.0);
        }
        repeated.push_back(one_per_date[i]);
    }
    ASSERT_EQ(repeated.size(), one_per_date.size() + 2);

    for (int warmup : {0, 2}) {
        const auto want = calc_.calculate_all_metrics(one_per_date, {}, warmup, kUnitPoints);
        const auto got = calc_.calculate_all_metrics(repeated, {}, warmup, kUnitPoints);
        EXPECT_DOUBLE_EQ(got.total_return, want.total_return) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.volatility, want.volatility) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.sharpe_ratio, want.sharpe_ratio) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.sortino_ratio, want.sortino_ratio) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.downside_volatility, want.downside_volatility) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.max_drawdown, want.max_drawdown) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.calmar_ratio, want.calmar_ratio) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.var_95, want.var_95) << "warmup " << warmup;
        EXPECT_DOUBLE_EQ(got.cvar_95, want.cvar_95) << "warmup " << warmup;
        EXPECT_EQ(got.drawdown_curve.size(), want.drawdown_curve.size()) << "warmup " << warmup;
        EXPECT_EQ(got.monthly_returns, want.monthly_returns) << "warmup " << warmup;
    }
    // The hand figure, so the pair above cannot agree on a wrong number: the deepest fall is
    // 102 -> 101 (104 -> 103 is shallower), and 80 is on no date.
    EXPECT_NEAR(calc_.calculate_all_metrics(repeated, {}, 0, kUnitPoints).max_drawdown, 1.0 / 102.0, 1e-12);
}

// A curve that repeats no date is read exactly as before: two rows a few hours apart across a
// UTC midnight are two dates, and a row at 23:59:59 belongs to its own date.
TEST_F(BacktestMetricsCalculatorTest, AllMetricsKeepEveryRowOfDistinctUtcDates) {
    std::vector<std::pair<Timestamp, double>> curve = {
        {date_at(2026, 1, 1) + std::chrono::hours(4), 100.0},
        {date_at(2026, 1, 1) + std::chrono::hours(24) - std::chrono::seconds(1) +
             std::chrono::hours(24),
         90.0},
        {date_at(2026, 1, 3) + std::chrono::hours(1), 99.0},
        {date_at(2026, 1, 4), 108.9},
    };
    const auto r = calc_.calculate_all_metrics(curve, {}, 0, kUnitPoints);
    EXPECT_NEAR(r.total_return, 0.089, 1e-12);
    EXPECT_NEAR(r.max_drawdown, 0.10, 1e-12);
    EXPECT_EQ(r.drawdown_curve.size(), curve.size());
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsPopulatesNonZeroFields) {
    std::vector<std::pair<Timestamp, double>> curve;
    for (int i = 0; i < 30; ++i) {
        double v = 100.0 + (i % 2 == 0 ? i : -i) * 0.5;
        curve.emplace_back(date_at(2026, 1, 1) + std::chrono::hours(24 * i), v);
    }
    auto r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/0, kUnitPoints);
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

TEST_F(BacktestMetricsCalculatorTest, SortinoNearTargetDustHitsSentinelNotAbsurdValue) {
    // Returns an epsilon below target leave ~1e-17 downside "dust". Pre-fix,
    // Sortino divided by the dust and reported an absurd finite magnitude;
    // post-fix the dust collapses to 0 and the degenerate-case sentinel applies.
    std::vector<double> returns(100, -1e-18);
    const double sortino =
        calc_.calculate_sortino_ratio(returns, /*trading_days=*/100, /*mar=*/0.0);
    EXPECT_LE(std::abs(sortino), 999.0);
    const double downside = calc_.calculate_downside_volatility(returns, 0.0);
    EXPECT_DOUBLE_EQ(downside, 0.0);
}
