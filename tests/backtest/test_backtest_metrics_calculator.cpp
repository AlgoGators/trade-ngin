#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <limits>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_metrics_calculator.hpp"
#include "trade_ngin/backtest/backtest_types.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/live/live_statistics_columns.hpp"
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

// The sessions a year of the two statistics grids, as the configuration carries them.
constexpr double kFuturesK = 311.0574;
constexpr double kEquityK = 252.0;
// A run whose every curve row is a session, at the equity grid's K.
const BacktestStatisticsGrid kOwnRows = BacktestStatisticsGrid::of_own_rows(kEquityK);

double sample_sd(const std::vector<double>& r) {
    double mean = 0.0;
    for (double x : r) mean += x;
    mean /= static_cast<double>(r.size());
    double squares = 0.0;
    for (double x : r) squares += (x - mean) * (x - mean);
    return std::sqrt(squares / static_cast<double>(r.size() - 1));
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

// The annualised return is the CAGR, (1 + R)^(K / n) - 1, never R x K / n.
//   | R    | n   | K   | geometric              | linear (wrong)  |
//   | 0.10 | 252 | 252 | 0.10                   | 0.10            |
//   | 0.10 | 126 | 252 | 1.1^2 - 1 = 0.21       | 0.20            |
//   | 0.10 | 126 | 315 | 1.1^2.5 - 1 = 0.269059 | 0.25            |
TEST_F(BacktestMetricsCalculatorTest, AnnualizedReturnIsGeometricOnSessionsAYearOverDays) {
    EXPECT_NEAR(calc_.calculate_annualized_return(0.10, 252, 252.0), 0.10, 1e-12);
    EXPECT_NEAR(calc_.calculate_annualized_return(0.10, 126, 252.0), 0.21, 1e-12);
    EXPECT_NEAR(calc_.calculate_annualized_return(0.10, 126, 315.0), std::pow(1.1, 2.5) - 1.0, 1e-12);
    EXPECT_NEAR(calc_.calculate_annualized_return(0.10, 126, 315.0), 0.269059, 1e-6);
    // A book that lost everything: no power of a non-positive growth.
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(-1.0, 126, 252.0), -1.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(-1.5, 126, 252.0), -1.0);
}

TEST_F(BacktestMetricsCalculatorTest, AnnualizedReturnNonPositiveDaysOrSessionsIsZero) {
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, 0, 252.0), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, -5, 252.0), 0.0);
    EXPECT_DOUBLE_EQ(calc_.calculate_annualized_return(0.10, 126, 0.0), 0.0);
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
    EXPECT_DOUBLE_EQ(calc_.calculate_volatility({}, kEquityK), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, VolatilityConstantSeriesIsZeroNotNaN) {
    // Regression: the one-pass E[r^2] - mean^2 formula cancels catastrophically
    // and rounds to a slightly negative variance for this input, so sqrt()
    // returned NaN instead of 0.
    auto vol = calc_.calculate_volatility({0.007, 0.007, 0.007, 0.007, 0.007}, kEquityK);
    EXPECT_DOUBLE_EQ(vol, 0.0);
}

// A constant series has no volatility, so its Sharpe ratio has no value. It is the
// calculator's own kNoRatio (the volatility itself is a clean 0, not rounding dust), not a 0
// that reads as a measured Sharpe.
TEST_F(BacktestMetricsCalculatorTest, SharpeOfAConstantSeriesHasNoValue) {
    auto sharpe = calc_.calculate_sharpe_ratio({0.007, 0.007, 0.007, 0.007, 0.007}, kEquityK, 0.0);
    EXPECT_FALSE(has_value(sharpe));
    EXPECT_TRUE(std::isnan(sharpe));
    EXPECT_DOUBLE_EQ(calc_.calculate_volatility({0.007, 0.007, 0.007, 0.007, 0.007}, kEquityK), 0.0);
    // One return: the sample deviation of one observation is 0.
    EXPECT_TRUE(std::isnan(calc_.calculate_sharpe_ratio({0.01}, kEquityK, 0.0)));
}

// Volatility is the SAMPLE standard deviation (n - 1) x sqrt(K).
//   returns 0.01, -0.01, 0.01, -0.01: mean 0, squares 0.0004
//   | divisor          | daily sd              | x sqrt(252) |
//   | n - 1 = 3        | sqrt(0.0004 / 3)      | 0.183303028 |
//   | n = 4 (wrong)    | sqrt(0.0004 / 4)      | 0.158745079 |
TEST_F(BacktestMetricsCalculatorTest, VolatilityIsTheSampleStandardDeviationAnnualised) {
    std::vector<double> r{0.01, -0.01, 0.01, -0.01};
    EXPECT_NEAR(calc_.calculate_volatility(r, 252.0), std::sqrt(0.0004 / 3.0) * std::sqrt(252.0), 1e-12);
    EXPECT_NEAR(calc_.calculate_volatility(r, 252.0), 0.183303028, 1e-9);
    EXPECT_GT(std::abs(calc_.calculate_volatility(r, 252.0) - 0.158745079), 1e-3)
        << "the population standard deviation (n) is not the stored convention";
    // One return has no sample deviation.
    EXPECT_DOUBLE_EQ(calc_.calculate_volatility({0.01}, 252.0), 0.0);
}

// The calculator holds no factor of its own: every annualised figure uses the sessions a year it
// is handed (the K of the grid the returns are taken on), on either grid's K.
//   returns 0.010, -0.004, 0.006, -0.012, 0.003, 0.000, 0.008: mean 0.0015714286,
//   sample sd 0.0076563077, downside sqrt((0.004^2 + 0.012^2) / 7) = 0.0047809144
//   | K        | volatility  | downside    | Sharpe      | Sortino     |
//   | 252      | 0.121540    | 0.075895    | 3.258183    | 5.217758    |
//   | 311.0574 | 0.135033    | 0.084320    | 3.619891    | 5.797009    |
TEST_F(BacktestMetricsCalculatorTest, EveryAnnualisedFigureUsesTheSessionsAYearHandedIn) {
    const std::vector<double> r = {0.010, -0.004, 0.006, -0.012, 0.003, 0.000, 0.008};
    double mean = 0.0, downside = 0.0;
    for (double x : r) {
        mean += x;
        if (x < 0.0) downside += x * x;
    }
    mean /= static_cast<double>(r.size());
    const double sd = sample_sd(r);
    const double dd = std::sqrt(downside / static_cast<double>(r.size()));
    for (const double k : {kEquityK, kFuturesK}) {
        EXPECT_NEAR(calc_.calculate_volatility(r, k), sd * std::sqrt(k), 1e-12) << k;
        EXPECT_NEAR(calc_.calculate_downside_volatility(r, k, 0.0), dd * std::sqrt(k), 1e-12) << k;
        EXPECT_NEAR(calc_.calculate_sharpe_ratio(r, k, 0.0), mean * k / (sd * std::sqrt(k)), 1e-9) << k;
        EXPECT_NEAR(calc_.calculate_sortino_ratio(r, k, 0.0), mean * k / (dd * std::sqrt(k)), 1e-9) << k;
        EXPECT_NEAR(calc_.calculate_risk_metrics(r, k).at("downside_volatility"), dd * std::sqrt(k), 1e-12)
            << k;
    }
    EXPECT_NEAR(calc_.calculate_volatility(r, kEquityK), 0.121540, 1e-6);
    EXPECT_NEAR(calc_.calculate_volatility(r, kFuturesK), 0.135033, 1e-6);
    EXPECT_NEAR(calc_.calculate_sharpe_ratio(r, kFuturesK, 0.0), 3.619891, 1e-6);
    EXPECT_NEAR(calc_.calculate_sortino_ratio(r, kFuturesK, 0.0), 5.797009, 1e-6);
    // The composite records the factor it applied: what the run metadata stores as statistics_K.
    const auto curve = linear_equity_curve(10, 100.0, 1.0);
    for (const double k : {kEquityK, kFuturesK}) {
        const auto all = calc_.calculate_all_metrics(curve, {}, 0, kUnitPoints,
                                                     BacktestStatisticsGrid::of_own_rows(k));
        EXPECT_DOUBLE_EQ(all.statistics_sessions_per_year, k);
        EXPECT_EQ(all.statistics_days, 9);
        EXPECT_NEAR(all.volatility,
                    sample_sd(calc_.calculate_returns_from_equity(curve)) * std::sqrt(k), 1e-12);
    }
}

TEST_F(BacktestMetricsCalculatorTest, DownsideVolatilityZeroWhenAllReturnsAboveTarget) {
    EXPECT_DOUBLE_EQ(calc_.calculate_downside_volatility({0.01, 0.02, 0.03}, kEquityK, 0.0), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, DownsideVolatilityCountsOnlyBelowTarget) {
    std::vector<double> r{-0.01, 0.01, -0.01, 0.01};
    // Two -0.01s contribute (0.0001 each), summed 0.0002 and averaged over ALL FOUR returns:
    // 0.00005; sqrt = 0.0070710678; * sqrt(252) = 0.112249722
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 252.0, 0.0), 0.112249722, 1e-9);
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 252.0, 0.0),
                std::sqrt(0.0002 / 4.0) * std::sqrt(252.0), 1e-12);
}

TEST_F(BacktestMetricsCalculatorTest, DownsideVolatilityAndSortinoAverageOverEveryReturn) {
    std::vector<double> r{0.01, -0.02, 0.03, -0.01};
    // Below target 0: -0.02 and -0.01, squares 0.0004 + 0.0001 = 0.0005, over 4 returns =
    // 0.000125; sqrt = 0.0111803399; * sqrt(252) = 0.177482393.
    EXPECT_NEAR(calc_.calculate_downside_volatility(r, 252.0, 0.0), 0.177482393, 1e-9);
    // mean = 0.0025, * 252 = 0.63; 0.63 / 0.177482393 = 3.549647870.
    EXPECT_NEAR(calc_.calculate_sortino_ratio(r, 252.0), 3.549647870, 1e-8);
}

TEST_F(BacktestMetricsCalculatorTest, SharpeOfAnEmptySeriesHasNoValue) {
    EXPECT_TRUE(std::isnan(calc_.calculate_sharpe_ratio({}, 252)));
}

TEST_F(BacktestMetricsCalculatorTest, SharpeWithoutPositiveSessionsAYearReturnsZero) {
    EXPECT_TRUE(std::isnan(calc_.calculate_sharpe_ratio({0.01, 0.02}, 0)));
    EXPECT_TRUE(std::isnan(calc_.calculate_sharpe_ratio({0.01, 0.02}, -1)));
}

TEST_F(BacktestMetricsCalculatorTest, SharpeNonZeroWhenVolatilityPositive) {
    std::vector<double> r{0.01, -0.01, 0.02, -0.01, 0.015, -0.005};
    EXPECT_NE(calc_.calculate_sharpe_ratio(r, 252), 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, SortinoOfAnEmptySeriesHasNoValue) {
    EXPECT_TRUE(std::isnan(calc_.calculate_sortino_ratio({}, 252)));
    EXPECT_TRUE(std::isnan(calc_.calculate_sortino_ratio({0.01}, 0)));
}

// With no return under the target there is no downside to divide by. The ratio has no value (not
// 999).
TEST_F(BacktestMetricsCalculatorTest, SortinoWithNoDownsideHasNoValue) {
    const double sortino = calc_.calculate_sortino_ratio({0.01, 0.02, 0.03}, 252);
    EXPECT_TRUE(std::isnan(sortino));
    EXPECT_FALSE(has_value(sortino));
}

TEST_F(BacktestMetricsCalculatorTest, SortinoWithNoDownsideHasNoValueWhateverTheSignOfTheMean) {
    // Returns all above target=-0.01 (no downside) but mean*252 is negative: no value, not 0.
    EXPECT_TRUE(std::isnan(calc_.calculate_sortino_ratio({-0.005, 0.001}, 252, -0.01)));
}

TEST_F(BacktestMetricsCalculatorTest, SortinoComputesWithDownsideVolatility) {
    // Mean must be non-zero for the result to be non-zero.
    std::vector<double> r{0.02, -0.01, 0.02, -0.01};  // mean = 0.005
    EXPECT_GT(calc_.calculate_sortino_ratio(r, 252), 0.0);
}

// No drawdown, no denominator, no value (not 999 or 0 by the return's sign).
TEST_F(BacktestMetricsCalculatorTest, CalmarWithNoDrawdownHasNoValue) {
    EXPECT_TRUE(std::isnan(calc_.calculate_calmar_ratio(0.10, 0.0)));
    EXPECT_TRUE(std::isnan(calc_.calculate_calmar_ratio(-0.10, 0.0)));
    EXPECT_TRUE(std::isnan(calc_.calculate_calmar_ratio(0.10, -0.05)));
    EXPECT_DOUBLE_EQ(calc_.calculate_calmar_ratio(0.10, 0.05), 2.0);
}

TEST_F(BacktestMetricsCalculatorTest, CalmarStandardCase) {
    EXPECT_DOUBLE_EQ(calc_.calculate_calmar_ratio(0.20, 0.10), 2.0);
}

// calmar = CAGR (geometric, K / n) / max_dd, through the composite. Own rows at K 252:
//   levels 100, 101, 100.5, 102, 103: n 4, R 0.03, max_dd 0.5 / 101 = 0.004950
//   | numerator                                 | calmar      |
//   | CAGR 1.03^(252 / 4) - 1 = 5.437914        | 1098.458585 |
//   | mean x 252 = 1.876064 (wrong)             | 378.965004  |
TEST_F(BacktestMetricsCalculatorTest, CalmarIsTheGeometricAnnualisedReturnOverTheMaximumDrawdown) {
    const std::vector<std::pair<Timestamp, double>> curve = {
        {date_at(2026, 1, 5), 100.0}, {date_at(2026, 1, 6), 101.0}, {date_at(2026, 1, 7), 100.5},
        {date_at(2026, 1, 8), 102.0}, {date_at(2026, 1, 9), 103.0}};
    const auto r = calc_.calculate_all_metrics(curve, {}, 0, kUnitPoints, kOwnRows);
    EXPECT_NEAR(r.max_drawdown, 0.5 / 101.0, 1e-12);
    EXPECT_NEAR(r.annualized_return, std::pow(1.03, 252.0 / 4.0) - 1.0, 1e-9);
    EXPECT_NEAR(r.annualized_return, 5.437914, 1e-6);
    EXPECT_NEAR(r.calmar_ratio, (std::pow(1.03, 252.0 / 4.0) - 1.0) / (0.5 / 101.0), 1e-6);
    EXPECT_NEAR(r.calmar_ratio, 1098.458585, 1e-5);
    // A falling book: the CAGR is negative where the mean return can be positive.
    //   levels 100, 150, 75, 90: returns +0.5, -0.5, +0.2 (mean +0.0667), R -0.10, max_dd 0.5
    const std::vector<std::pair<Timestamp, double>> falling = {
        {date_at(2026, 1, 5), 100.0}, {date_at(2026, 1, 6), 150.0}, {date_at(2026, 1, 7), 75.0},
        {date_at(2026, 1, 8), 90.0}};
    const auto f = calc_.calculate_all_metrics(falling, {}, 0, kUnitPoints, kOwnRows);
    EXPECT_NEAR(f.calmar_ratio, (std::pow(0.9, 252.0 / 3.0) - 1.0) / 0.5, 1e-9);
    EXPECT_LT(f.calmar_ratio, 0.0);
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

// CVaR includes the VaR observation: the mean of the worst floor(0.05 n) + 1 returns,
// the last of them being the one var_95 reads.
//   20 returns -0.10 .. 0.09: floor(0.05 x 20) = 1, VaR = the second worst = 0.09
//   | rule                              | CVaR                       |
//   | with the VaR observation          | (0.10 + 0.09) / 2 = 0.095  |
//   | the returns below it (wrong)      | 0.10                       |
TEST_F(BacktestMetricsCalculatorTest, Cvar95AveragesTheTailWithTheVarObservation) {
    std::vector<double> r;
    for (int i = -10; i <= 9; ++i) r.push_back(i * 0.01);
    EXPECT_NEAR(calc_.calculate_var_95(r), 0.09, 1e-9);
    EXPECT_NEAR(calc_.calculate_cvar_95(r), 0.095, 1e-9);
    EXPECT_GE(calc_.calculate_cvar_95(r), calc_.calculate_var_95(r)) << "the tail mean is at least the VaR";

    // 40 returns: floor(0.05 x 40) = 2; VaR is the third worst (0.03); CVaR the mean of the worst
    // three, (0.10 + 0.05 + 0.03) / 3 = 0.06 (the two below the VaR observation alone: 0.075).
    std::vector<double> forty(37, 0.01);
    forty.push_back(-0.10);
    forty.push_back(-0.05);
    forty.push_back(-0.03);
    EXPECT_NEAR(calc_.calculate_var_95(forty), 0.03, 1e-12);
    EXPECT_NEAR(calc_.calculate_cvar_95(forty), 0.06, 1e-12);
}

TEST_F(BacktestMetricsCalculatorTest, Cvar95OfATinyInputIsItsWorstReturnTheVarObservation) {
    // floor(3 x 0.05) = 0: the VaR observation is the worst return, and CVaR is that one return
    std::vector<double> r{-0.05, 0.0, 0.05};
    EXPECT_NEAR(calc_.calculate_var_95(r), 0.05, 1e-9);
    EXPECT_NEAR(calc_.calculate_cvar_95(r), 0.05, 1e-9);
    EXPECT_NEAR(calc_.calculate_cvar_95({-0.02}), 0.02, 1e-12);
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
    EXPECT_TRUE(std::isnan(s.profit_factor)) << "no losing trade: no value (T-8D R12), it was 999";
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

// A trade is scored in DOLLARS, the price move times the symbol's point value (the metadata's
// contract size, the figure the equity curve books with). Real fills of the futures book (MES n=70
// and ZN n=128 of one run):
//
//   | row            | closed | entry       | exit       | points    | point value | gross     | cost        | trade        |
//   | MES SELL 1     | 1      | 6785.25     | 6641.75    | -143.5    | 5           | -717.50   | 2.14618282  | -719.646183  |
//   | ZN  SELL 2     | 2      | 113.2578125 | 112.390625 | -0.8671875| 1000        | -1734.375 | 11.00661929 | -1745.381619 |
//
// A price term in points would score MES -145.646183, ZN -12.740994.
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

// A fill that takes the position through zero closes what was held and opens the
// remainder at ITS price.
//
//   | day | fill          | position | entry | trade                              |
//   | 0   | BUY 2 at 100  | +2       | 100   |                                    |
//   | 1   | SELL 3 at 110 | -1       | 110   | closes 2: 2 x (110 - 100) = +20    |
//   | 2   | BUY 1 at 105  | 0        |       | closes the short: 110 - 105 = +5   |
//
// Keeping the long's entry 100 for the short would score 100 - 105 = -5.
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

// A position under 1e-9 after a fill is flat. Fractional share
// quantities summed in binary leave a residue after a full close (0.3 - 0.1 - 0.2 = -2.8e-17):
//
//   | day | fill            | position            | trade                                  |
//   | 0   | BUY 0.3 at 100  | 0.3                 |                                        |
//   | 1   | SELL 0.1 at 110 | 0.2                 | 0.1 x 10 = +1                          |
//   | 2   | SELL 0.2 at 110 | flat (-2.8e-17)     | 0.2 x 10 = +2                          |
//   | 5   | BUY 1 at 200    | +1 at 200           | none: it opens                         |
//   | 6   | SELL 1 at 210   | 0                   | 210 - 200 = +10                        |
//
// Taken as a short position, the residue would make the BUY of day 5 "close" it (a fourth trade, a
// loss of 2.8e-15) and measure the last SELL from 100 (+110).
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
    EXPECT_TRUE(std::isnan(s.profit_factor));
    EXPECT_NEAR(calc_.calculate_symbol_pnl(execs, kUnitPoints)["X"], 13.0, 1e-9);
}

namespace {

// Real rows of a two-sleeve futures backtest (the backtest.executions ids are in the last column),
// three contracts. The statistics are the BOOK's: the STRATEGY rows of one contract on one day are
// first netted into the account's fill. T = TREND_FOLLOWING, F = TREND_FOLLOWING_FAST; cost = own
// cost - adjustment.
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

// The trade statistics are the book's. The table above: a full cross is no fill and no trade; a
// partial offset is one fill of the net size, charged the sum of its rows' costs after netting; two
// sleeves on one side are one fill. Scoring every sleeve row as a fill of one tracker, in points,
// would score trades on the crossed rows (the 6C cross a "win" of 0.00455 or 0.0091 by the order of
// the sleeves) and read price points as dollars.
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

// The order is the calculator's own (by day, then contract), so the order the rows
// arrive in changes nothing. The same rows reversed, and in seven seeded shuffles, give the same
// statistics to the last bit and the same account fills.
// A walk in the input order would let the first row of the reversed rows "close".
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
    backtest::BacktestResults r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/3, kUnitPoints, kOwnRows);
    EXPECT_GT(r.total_return, 0.0);
    // total_return ~= (109-103)/103
    EXPECT_NEAR(r.total_return, (109.0 - 103.0) / 103.0, 1e-9);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsEmptyCurveReturnsDefaults) {
    auto r = calc_.calculate_all_metrics({}, {}, 0, kUnitPoints, kOwnRows);
    EXPECT_DOUBLE_EQ(r.total_return, 0.0);
    EXPECT_DOUBLE_EQ(r.max_drawdown, 0.0);
}

TEST_F(BacktestMetricsCalculatorTest, AllMetricsWarmupGreaterThanSizePassesFullCurveThrough) {
    // filter_warmup_period treats warmup>=size the same as warmup<=0: it returns
    // the unfiltered curve so the metrics still compute on the full series.
    auto curve = linear_equity_curve(3, 100.0, 1.0);
    auto r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/100, kUnitPoints, kOwnRows);
    EXPECT_NEAR(r.total_return, (102.0 - 100.0) / 100.0, 1e-9);
}

// A UTC date the curve carries twice counts once, by its LAST row: the metrics of the repeated
// curve equal the metrics of the curve with one row per date. Not de-duplicated, the repeated
// date would add a return of its own (two more observations here, one of them a 20 percent
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
        const auto want = calc_.calculate_all_metrics(one_per_date, {}, warmup, kUnitPoints, kOwnRows);
        const auto got = calc_.calculate_all_metrics(repeated, {}, warmup, kUnitPoints, kOwnRows);
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
    EXPECT_NEAR(calc_.calculate_all_metrics(repeated, {}, 0, kUnitPoints, kOwnRows).max_drawdown, 1.0 / 102.0, 1e-12);
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
    const auto r = calc_.calculate_all_metrics(curve, {}, 0, kUnitPoints, kOwnRows);
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
    auto r = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/0, kUnitPoints, kOwnRows);
    EXPECT_NE(r.volatility, 0.0);
    EXPECT_FALSE(r.drawdown_curve.empty());
    EXPECT_GE(r.max_drawdown, 0.0);
}

// ===== The statistics grid =====

namespace {

// A futures run's rows around its warm-up, with a THIN Sunday-to-Friday date (Tue 01-06: too few
// symbols printed, so it is not a grid date) and a SATURDAY point (01-10, the run's trough).
//   | date       | day | level | on the grid          | grid return        |
//   | 2026-01-01 | Thu | 100   | warm-up row, dropped |                    |
//   | 2026-01-02 | Fri | 100   | the base             |                    |
//   | 2026-01-04 | Sun | 102   | yes                  | 102/100 - 1        |
//   | 2026-01-05 | Mon | 101   | yes                  | 101/102 - 1        |
//   | 2026-01-06 | Tue |  99   | THIN: no row         | folded into 01-07  |
//   | 2026-01-07 | Wed | 103   | yes                  | 103/101 - 1        |
//   | 2026-01-08 | Thu | 104   | yes                  | 104/103 - 1        |
//   | 2026-01-09 | Fri | 102   | yes                  | 102/104 - 1        |
//   | 2026-01-10 | Sat |  96   | SATURDAY: no row     | folded into 01-11  |
//   | 2026-01-11 | Sun | 105   | yes                  | 105/102 - 1        |
//   | 2026-01-12 | Mon | 106   | yes                  | 106/105 - 1        |
std::vector<std::pair<Timestamp, double>> thin_and_saturday_curve() {
    return {{date_at(2026, 1, 1), 100.0}, {date_at(2026, 1, 2), 100.0}, {date_at(2026, 1, 4), 102.0},
            {date_at(2026, 1, 5), 101.0}, {date_at(2026, 1, 6), 99.0},  {date_at(2026, 1, 7), 103.0},
            {date_at(2026, 1, 8), 104.0}, {date_at(2026, 1, 9), 102.0}, {date_at(2026, 1, 10), 96.0},
            {date_at(2026, 1, 11), 105.0}, {date_at(2026, 1, 12), 106.0}};
}

// The grid as the coordinator hands it: the dates of the WINDOW that pass the predicate, the
// warm-up's and one after the curve's end included, in no order.
BacktestStatisticsGrid thin_and_saturday_grid() {
    return BacktestStatisticsGrid::of_dates(
        kFuturesK, {"2026-01-12", "2026-01-13", "2026-01-01", "2026-01-02", "2026-01-04", "2026-01-05",
                    "2026-01-07", "2026-01-08", "2026-01-09", "2026-01-11", "2025-12-31"});
}

const std::vector<double> kGridLevels = {100.0, 102.0, 101.0, 103.0, 104.0, 102.0, 105.0, 106.0};

}  // namespace

// The fold itself: the Saturday point and the thin date are no rows, their levels are not read,
// and their P&L is inside the next grid return.
TEST_F(BacktestMetricsCalculatorTest, TheGridCurveDropsASaturdayPointAndAThinDateAndKeepsTheirPnl) {
    auto curve = thin_and_saturday_curve();
    curve.erase(curve.begin());  // the calculator folds the rows after the warm-up
    const auto folded = calc_.statistics_grid_curve(curve, thin_and_saturday_grid());
    ASSERT_EQ(folded.size(), kGridLevels.size());
    for (size_t i = 0; i < folded.size(); ++i) {
        EXPECT_DOUBLE_EQ(folded[i].second, kGridLevels[i]) << i;
    }
    EXPECT_EQ(folded.front().first, date_at(2026, 1, 2)) << "the base is the first row";
    EXPECT_EQ(folded[3].first, date_at(2026, 1, 7));
    EXPECT_EQ(folded[6].first, date_at(2026, 1, 11));
    // Nothing is dropped: the grid returns chain to the level of the last row.
    double chained = 1.0;
    for (double r : calc_.calculate_returns_from_equity(folded)) chained *= 1.0 + r;
    EXPECT_NEAR(chained, 106.0 / 100.0, 1e-12);

    // A base dated off the grid is still the base; a curve ending off the grid ends at its last
    // grid date; a grid date with no row repeats the level before it; own rows: the curve itself.
    const std::vector<std::pair<Timestamp, double>> ragged = {
        {date_at(2026, 1, 3), 100.0}, {date_at(2026, 1, 4), 101.0}, {date_at(2026, 1, 6), 103.0},
        {date_at(2026, 1, 10), 90.0}};
    const auto short_fold = calc_.statistics_grid_curve(
        ragged, BacktestStatisticsGrid::of_dates(kFuturesK, {"2026-01-04", "2026-01-05", "2026-01-06"}));
    ASSERT_EQ(short_fold.size(), 4u);
    EXPECT_DOUBLE_EQ(short_fold[0].second, 100.0);
    EXPECT_DOUBLE_EQ(short_fold[1].second, 101.0);
    EXPECT_DOUBLE_EQ(short_fold[2].second, 101.0) << "01-05 has no row: the level before it";
    EXPECT_EQ(short_fold[2].first, date_at(2026, 1, 5));
    EXPECT_DOUBLE_EQ(short_fold[3].second, 103.0);
    EXPECT_EQ(calc_.statistics_grid_curve(ragged, kOwnRows), ragged);
    EXPECT_EQ(calc_.statistics_grid_curve(ragged, BacktestStatisticsGrid::of_dates(kFuturesK, {})).size(), 1u)
        << "an empty grid leaves the base and no return";
}

// A backtest with a thin Sunday-to-Friday date and a Saturday point, through the
// composite. The grid statistics read the seven grid returns at K = 311.0574; total_return and
// the drawdown curve read every row.
//   | figure        | on the grid (stored)   | on every row, K 311.0574 | every row, n, K 252 (before) |
//   | n             | 7                      | 9                        | 9                            |
//   | volatility    | 0.305803               | 0.759735                 | 0.644712                     |
//   | sharpe_ratio  | 8.633205               | 2.990030                 |                              |
//   | downside vol  | 0.143892               | 0.386326                 |                              |
//   | sortino_ratio | 18.347538              | 5.880092                 |                              |
//   | max_drawdown  | 2/104 = 0.019231       | 8/104 = 0.076923 (the Saturday trough)                  |
//   | var_95        | 2/104 = 0.019231       | 6/102 = 0.058824 (the Saturday fall)                    |
//   | total_return  | 0.06                   | 0.06                     | 0.06                         |
TEST_F(BacktestMetricsCalculatorTest, AThinDateAndASaturdayPointAreNoObservationsOfTheGridStatistics) {
    const auto r = calc_.calculate_all_metrics(thin_and_saturday_curve(), {}, /*warmup_days=*/1,
                                               kUnitPoints, thin_and_saturday_grid());
    std::vector<double> grid_returns;
    for (size_t i = 1; i < kGridLevels.size(); ++i) {
        grid_returns.push_back((kGridLevels[i] - kGridLevels[i - 1]) / kGridLevels[i - 1]);
    }
    double mean = 0.0, downside = 0.0;
    for (double x : grid_returns) {
        mean += x;
        if (x < 0.0) downside += x * x;
    }
    mean /= 7.0;
    const double sd = sample_sd(grid_returns);
    const double dd = std::sqrt(downside / 7.0);

    EXPECT_EQ(r.statistics_days, 7);
    EXPECT_DOUBLE_EQ(r.statistics_sessions_per_year, kFuturesK);
    EXPECT_NEAR(r.volatility, sd * std::sqrt(kFuturesK), 1e-12);
    EXPECT_NEAR(r.volatility, 0.305803, 1e-6);
    EXPECT_NEAR(r.sharpe_ratio, mean * kFuturesK / (sd * std::sqrt(kFuturesK)), 1e-9);
    EXPECT_NEAR(r.sharpe_ratio, 8.633205, 1e-6);
    EXPECT_NEAR(r.downside_volatility, dd * std::sqrt(kFuturesK), 1e-12);
    EXPECT_NEAR(r.downside_volatility, 0.143892, 1e-6);
    EXPECT_NEAR(r.sortino_ratio, mean * kFuturesK / (dd * std::sqrt(kFuturesK)), 1e-9);
    EXPECT_NEAR(r.sortino_ratio, 18.347538, 1e-6);
    EXPECT_NEAR(r.max_drawdown, 2.0 / 104.0, 1e-12) << "the Saturday trough (8/104) is no grid level";
    // calmar: the CAGR on the grid, 1.06^(311.0574 / 7) - 1 = 12.320206, over 2/104: 640.650727
    // (on every row, n 9: 6.492377 / (8/104) = 84.400902).
    EXPECT_NEAR(r.annualized_return, std::pow(1.06, kFuturesK / 7.0) - 1.0, 1e-9);
    EXPECT_NEAR(r.calmar_ratio, (std::pow(1.06, kFuturesK / 7.0) - 1.0) / (2.0 / 104.0), 1e-6);
    EXPECT_NEAR(r.calmar_ratio, 640.650727, 1e-4);
    EXPECT_NEAR(r.var_95, 2.0 / 104.0, 1e-12) << "the Saturday fall (6/102) is no grid return";
    EXPECT_NEAR(r.cvar_95, 2.0 / 104.0, 1e-12) << "seven returns: the worst one is the VaR observation";
    // Levels, on every row: the Saturday point and the thin date are still rows of the curve.
    EXPECT_NEAR(r.total_return, 0.06, 1e-12);
    ASSERT_EQ(r.drawdown_curve.size(), 10u);
    EXPECT_NEAR(r.drawdown_curve[7].second, 8.0 / 104.0, 1e-12) << "the drawdown curve keeps the row";

    // The same rows read as their own grid (every row an observation) are a different series.
    const auto rows = calc_.calculate_all_metrics(thin_and_saturday_curve(), {}, 1, kUnitPoints,
                                                  BacktestStatisticsGrid::of_own_rows(kFuturesK));
    EXPECT_EQ(rows.statistics_days, 9);
    EXPECT_NEAR(rows.volatility, 0.759735, 1e-6);
    EXPECT_NEAR(rows.max_drawdown, 8.0 / 104.0, 1e-12);
    EXPECT_DOUBLE_EQ(rows.total_return, r.total_return);
}

// ONE P&L series through both calculators gives one number per
// statistic. The backtest calculator is handed the curve and the grid; the live helper the same
// rows as stored levels, the same grid dates, the base as the book's start (units: the live
// helper is in percent).
//   | statistic          | backtest (fraction) | live helper (percent) |
//   | n                  | 7                   | 7                     |
//   | volatility         | 0.305803            | 30.5803               |
//   | sharpe_ratio       | 8.633205            | 8.633205              |
//   | downside           | 0.143892            | 14.3892               |
//   | sortino_ratio      | 18.347538           | 18.347538             |
//   | max_drawdown       | 0.019231            | 1.9231                |
//   | annualised return  | 12.320206           | 1232.0206             |
//   | calmar_ratio       | 640.650727          | (annualised / max_drawdown, no live column) |
TEST_F(BacktestMetricsCalculatorTest, BothCalculatorsGiveOneNumberForOnePnlSeries) {
    const auto curve = thin_and_saturday_curve();
    const auto grid = thin_and_saturday_grid();
    const auto bt = calc_.calculate_all_metrics(curve, {}, /*warmup_days=*/1, kUnitPoints, grid);

    std::vector<DatedLevel> stored;
    for (size_t i = 1; i < curve.size(); ++i) {  // the rows after the warm-up
        stored.push_back({core::format_utc_date(curve[i].first), curve[i].second});
    }
    const StatisticsSeries series =
        build_statistics_series(stored, grid.dates, "2026-01-02", 100.0, "2026-01-12");
    const HistoricalMetrics live =
        LiveHistoricalMetricsCalculator().calculate(series, grid.sessions_per_year, 0);

    EXPECT_EQ(bt.statistics_days, live.total_days);
    EXPECT_EQ(live.total_days, 7);
    EXPECT_NEAR(bt.volatility * 100.0, live.volatility, 1e-10);
    EXPECT_NEAR(bt.sharpe_ratio, live.sharpe_ratio, 1e-10);
    EXPECT_NEAR(bt.downside_volatility * 100.0, live.downside_deviation, 1e-10);
    EXPECT_NEAR(bt.sortino_ratio, live.sortino_ratio, 1e-10);
    EXPECT_NEAR(bt.max_drawdown * 100.0, live.max_drawdown, 1e-10);
    EXPECT_NEAR(live.volatility, 30.5803, 1e-4);
    EXPECT_NEAR(live.sharpe_ratio, 8.633205, 1e-6);
    // The annualised return: one function behind both (grid_annualized_return).
    EXPECT_NEAR(bt.annualized_return * 100.0, live.total_annualized_return, 1e-8);
    EXPECT_NEAR(live.total_annualized_return, 1232.0206, 1e-3);
    EXPECT_NEAR(bt.calmar_ratio, live.total_annualized_return / live.max_drawdown, 1e-8);
}

// The backtest half: a run has ONE as-of date, its last curve row, and
// every grid statistic reads the grid rows up to it. A run whose last row is off the grid (a
// Saturday point) therefore ends its series at the last grid date: every statistic of the series
// equals that of the same run stopped on the Friday; the levels (total_return, the drawdown
// curve) are facts of every row and keep the Saturday.
//   | row        | day | level | in the grid series      | in total_return, the drawdown curve |
//   | 2026-01-01 | Thu | 100   | warm-up                 | warm-up                             |
//   | 2026-01-02 | Fri | 100   | the base                | the base                            |
//   | 2026-01-04 | Sun | 102   | 102/100 - 1             | yes                                 |
//   | 2026-01-05 | Mon | 101   | 101/102 - 1             | yes                                 |
//   | 2026-01-06 | Tue |  99   | thin: folded into 01-07 | yes                                 |
//   | 2026-01-07 | Wed | 103   | 103/101 - 1             | yes                                 |
//   | 2026-01-08 | Thu | 104   | 104/103 - 1             | yes                                 |
//   | 2026-01-09 | Fri | 102   | 102/104 - 1, the last   | yes                                 |
//   | 2026-01-10 | Sat |  96   | no row, in no return    | yes: 96/100 - 1 = -0.04, (104-96)/104 |
TEST_F(BacktestMetricsCalculatorTest, ARunEndingOnAnOffGridRowEndsEveryGridStatisticAtItsLastGridDate) {
    auto saturday_end = thin_and_saturday_curve();
    saturday_end.resize(9);  // through Sat 2026-01-10
    auto friday_end = saturday_end;
    friday_end.pop_back();  // through Fri 2026-01-09
    const auto grid = thin_and_saturday_grid();
    const auto sat = calc_.calculate_all_metrics(saturday_end, {}, /*warmup_days=*/1, kUnitPoints, grid);
    const auto fri = calc_.calculate_all_metrics(friday_end, {}, /*warmup_days=*/1, kUnitPoints, grid);

    // The series: the same five grid returns, the last one dated the Friday.
    const auto base_and_after = std::vector<std::pair<Timestamp, double>>(saturday_end.begin() + 1,
                                                                          saturday_end.end());
    const StatisticsSeries series = calc_.statistics_series(base_and_after, grid);
    ASSERT_EQ(series.size(), 5);
    EXPECT_EQ(series.dates.back(), "2026-01-09");
    EXPECT_DOUBLE_EQ(series.levels.back(), 102.0);
    EXPECT_EQ(sat.statistics_days, 5);
    EXPECT_EQ(fri.statistics_days, 5);

    // Every statistic of the series: one number, with or without the Saturday row.
    EXPECT_DOUBLE_EQ(sat.volatility, fri.volatility);
    EXPECT_DOUBLE_EQ(sat.sharpe_ratio, fri.sharpe_ratio);
    EXPECT_DOUBLE_EQ(sat.sortino_ratio, fri.sortino_ratio);
    EXPECT_DOUBLE_EQ(sat.downside_volatility, fri.downside_volatility);
    EXPECT_DOUBLE_EQ(sat.max_drawdown, fri.max_drawdown);
    EXPECT_DOUBLE_EQ(sat.annualized_return, fri.annualized_return);
    EXPECT_DOUBLE_EQ(sat.calmar_ratio, fri.calmar_ratio);
    EXPECT_DOUBLE_EQ(sat.var_95, fri.var_95);
    EXPECT_DOUBLE_EQ(sat.cvar_95, fri.cvar_95);
    EXPECT_DOUBLE_EQ(sat.beta, fri.beta);
    EXPECT_DOUBLE_EQ(sat.correlation, fri.correlation);
    EXPECT_GT(sat.volatility, 0.0);
    EXPECT_NEAR(sat.max_drawdown, 2.0 / 104.0, 1e-12) << "the Saturday trough (8/104) is no grid level";
    EXPECT_NEAR(sat.annualized_return, std::pow(1.02, kFuturesK / 5.0) - 1.0, 1e-9)
        << "the CAGR ends on the Friday level 102, not on the Saturday's 96";
    // The statistics of migration 037 read the same series.
    ASSERT_TRUE(sat.statistics_columns.worst_day.has_value());
    EXPECT_DOUBLE_EQ(*sat.statistics_columns.worst_day, *fri.statistics_columns.worst_day);
    EXPECT_NEAR(*sat.statistics_columns.worst_day, 102.0 / 104.0 - 1.0, 1e-12);
    EXPECT_EQ(sat.statistics_columns.worst_day_date, fri.statistics_columns.worst_day_date);
    EXPECT_EQ(sat.statistics_columns.calendar_year_returns, fri.statistics_columns.calendar_year_returns);
    EXPECT_EQ(sat.statistics_columns.losing_years, fri.statistics_columns.losing_years);
    EXPECT_EQ(sat.statistics_columns.monthly_skew, fri.statistics_columns.monthly_skew);

    // The levels are facts of every row: they keep the Saturday.
    EXPECT_NEAR(sat.total_return, 96.0 / 100.0 - 1.0, 1e-12);
    EXPECT_NEAR(fri.total_return, 102.0 / 100.0 - 1.0, 1e-12);
    ASSERT_EQ(sat.drawdown_curve.size(), 8u);
    ASSERT_EQ(fri.drawdown_curve.size(), 7u);
    EXPECT_NEAR(sat.drawdown_curve.back().second, 8.0 / 104.0, 1e-12);
}

// The same as-of date decides which calendar months and years are complete (migration 037: "a
// month is complete when its last calendar day is on or before the run's last curve date"). A run
// ending on Saturday 2026-01-31, an off-grid row, counts January: every grid date of the month is
// in its series, and the month's return is the product of its grid returns, the last one dated
// Friday 01-30, so the Saturday row's level is in no monthly figure. The same run stopped on the
// Friday leaves January partial. The series, and so every other statistic, is the same.
TEST_F(BacktestMetricsCalculatorTest, AMonthEndingOnAnOffGridRowIsCompleteOnItsGridReturns) {
    // The base on Wed 2025-12-31; one row per Sunday-to-Friday date of January 2026, each a grid
    // date; then the Saturday point of 01-31.
    std::vector<std::pair<Timestamp, double>> curve = {{date_at(2025, 12, 31), 100.0}};
    std::vector<std::string> grid_dates;
    double level = 100.0;
    for (int day = 1; day <= 30; ++day) {
        const Timestamp t = date_at(2026, 1, day);
        const std::time_t tt = std::chrono::system_clock::to_time_t(t);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        if (tm.tm_wday == 6) continue;  // a Saturday is never a grid date
        level *= (day % 3 == 0) ? 0.995 : 1.004;
        curve.emplace_back(t, level);
        grid_dates.push_back(core::format_utc_date(t));
    }
    const double friday_level = level;
    auto saturday_end = curve;
    saturday_end.emplace_back(date_at(2026, 1, 31), friday_level * 0.9);  // a large Saturday move
    const auto grid = BacktestStatisticsGrid::of_dates(kFuturesK, grid_dates);

    const StatisticsSeries on_friday = calc_.statistics_series(curve, grid);
    const StatisticsSeries on_saturday = calc_.statistics_series(saturday_end, grid);
    EXPECT_EQ(on_saturday.dates, on_friday.dates);
    EXPECT_EQ(on_saturday.levels, on_friday.levels);
    EXPECT_EQ(on_saturday.dates.back(), "2026-01-30");

    const auto months_friday = complete_month_returns(on_friday, "2025-12-31", "2026-01-30");
    const auto months_saturday = complete_month_returns(on_saturday, "2025-12-31", "2026-01-31");
    EXPECT_TRUE(months_friday.empty()) << "January is partial on its 30th";
    ASSERT_EQ(months_saturday.size(), 1u);
    EXPECT_EQ(months_saturday[0].month, "2026-01");
    EXPECT_NEAR(months_saturday[0].return_pct, (friday_level / 100.0 - 1.0) * 100.0, 1e-9)
        << "the month's return ends on the Friday level: the Saturday row is in no grid return";
}

// Through the composite: a run that only rises has no downside return, no drawdown and
// (with one winning trade) no losing trade. Its three ratios have no value; its Sharpe ratio,
// which has a denominator, is a number. A flat run has no volatility either.
//   | curve               | sharpe | sortino | calmar | profit factor |
//   | 100, 101, 103, 104  | number | none    | none   | none (1 winning trade, no loss) |
//   | 100, 100, 100, 100  | none   | none    | none   | none (no trade)                 |
TEST_F(BacktestMetricsCalculatorTest, ARatioWithNoDenominatorHasNoValueInTheResults) {
    const std::vector<std::pair<Timestamp, double>> rising = {
        {date_at(2026, 1, 5), 100.0}, {date_at(2026, 1, 6), 101.0}, {date_at(2026, 1, 7), 103.0},
        {date_at(2026, 1, 8), 104.0}};
    const std::vector<ExecutionReport> one_win = {
        make_exec("ES", Side::BUY, 1.0, 100.0, date_at(2026, 1, 5)),
        make_exec("ES", Side::SELL, 1.0, 104.0, date_at(2026, 1, 8))};
    const auto up = calc_.calculate_all_metrics(rising, one_win, /*warmup_days=*/0, kUnitPoints, kOwnRows);
    EXPECT_TRUE(has_value(up.sharpe_ratio));
    EXPECT_GT(up.sharpe_ratio, 0.0);
    EXPECT_TRUE(std::isnan(up.sortino_ratio));
    EXPECT_TRUE(std::isnan(up.calmar_ratio));
    EXPECT_TRUE(std::isnan(up.profit_factor));
    EXPECT_EQ(up.total_trades, 1);
    EXPECT_DOUBLE_EQ(up.max_drawdown, 0.0);
    EXPECT_DOUBLE_EQ(up.downside_volatility, 0.0);
    EXPECT_NEAR(up.total_return, 0.04, 1e-12);

    const std::vector<std::pair<Timestamp, double>> flat = {
        {date_at(2026, 1, 5), 100.0}, {date_at(2026, 1, 6), 100.0}, {date_at(2026, 1, 7), 100.0},
        {date_at(2026, 1, 8), 100.0}};
    const auto none = calc_.calculate_all_metrics(flat, {}, /*warmup_days=*/0, kUnitPoints, kOwnRows);
    EXPECT_TRUE(std::isnan(none.sharpe_ratio));
    EXPECT_TRUE(std::isnan(none.sortino_ratio));
    EXPECT_TRUE(std::isnan(none.calmar_ratio));
    EXPECT_TRUE(std::isnan(none.profit_factor));
    EXPECT_DOUBLE_EQ(none.volatility, 0.0);
    EXPECT_DOUBLE_EQ(none.total_return, 0.0);

    // A run with a loss on each denominator carries four numbers.
    const auto both = calc_.calculate_all_metrics(thin_and_saturday_curve(), {}, 1, kUnitPoints,
                                                  thin_and_saturday_grid());
    EXPECT_TRUE(has_value(both.sharpe_ratio));
    EXPECT_TRUE(has_value(both.sortino_ratio));
    EXPECT_TRUE(has_value(both.calmar_ratio));
}

// The console line of a ratio: the number as the stream is set, "n/a" when it has no value.
TEST_F(BacktestMetricsCalculatorTest, AConsoleLinePrintsNaForARatioWithNoValue) {
    std::ostringstream os;
    os << "Sortino Ratio: " << ratio_or_na(1.786723) << " | " << std::fixed << std::setprecision(3)
       << ratio_or_na(2.306329) << " | " << ratio_or_na(kNoRatio) << " | "
       << ratio_or_na(std::numeric_limits<double>::infinity());
    EXPECT_EQ(os.str(), "Sortino Ratio: 1.78672 | 2.306 | n/a | n/a");
    std::ostringstream plain;
    plain << 1.786723;
    EXPECT_EQ(plain.str(), "1.78672") << "a number is printed exactly as the stream prints it";
}

// ===== NaN / degeneracy guards (batch-2 metrics hardening) =====

TEST_F(BacktestMetricsCalculatorTest, NanEquityPointExcludedFromMonthlyReturns) {
    // NaN in the middle of the curve: neither the NaN-as-current nor the
    // NaN-as-previous transition may contribute. `prev <= 0.0` is false for
    // NaN, so an unguarded NaN period_return would poison the month's total.
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
    // Returns an epsilon below target leave ~1e-17 downside "dust". Dividing by
    // the dust would report an absurd finite magnitude; the dust collapses to 0
    // and the ratio has no value.
    std::vector<double> returns(100, -1e-18);
    const double sortino =
        calc_.calculate_sortino_ratio(returns, /*sessions_per_year=*/kEquityK, /*mar=*/0.0);
    EXPECT_TRUE(std::isnan(sortino));
    const double downside = calc_.calculate_downside_volatility(returns, kEquityK, 0.0);
    EXPECT_DOUBLE_EQ(downside, 0.0);
}
