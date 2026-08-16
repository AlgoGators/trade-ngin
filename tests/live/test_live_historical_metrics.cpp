// Coverage for live_historical_metrics.cpp. Pure-math calculator with no
// external deps — easy to test against hand-computed expected values.

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

// Static helpers are private; reach them via the standard pattern.
#include <string>
#define private public
#include "trade_ngin/live/live_historical_metrics.hpp"
#undef private

using namespace trade_ngin;

class LiveHistoricalMetricsTest : public ::testing::Test {};

// ===== calculate_mean =====

TEST_F(LiveHistoricalMetricsTest, MeanOfEmptyIsZero) {
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_mean({}), 0.0);
}

TEST_F(LiveHistoricalMetricsTest, MeanOfSingleValueIsThatValue) {
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_mean({3.5}), 3.5);
}

TEST_F(LiveHistoricalMetricsTest, MeanOfMultipleValuesIsArithmeticAverage) {
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_mean({1, 2, 3, 4, 5}), 3.0);
}

// ===== calculate_annualized_volatility =====

TEST_F(LiveHistoricalMetricsTest, VolatilityOfFewerThanTwoIsZero) {
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_annualized_volatility({}), 0.0);
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_annualized_volatility({1.0}), 0.0);
}

TEST_F(LiveHistoricalMetricsTest, VolatilityScalesBySqrt252) {
    // Two-element series: returns 1.0 and -1.0. Mean=0, variance=1, daily_std=1.
    // Annualized = sqrt(252) ≈ 15.875
    auto v = LiveHistoricalMetricsCalculator::calculate_annualized_volatility({1.0, -1.0});
    EXPECT_NEAR(v, std::sqrt(252.0), 1e-9);
}

TEST_F(LiveHistoricalMetricsTest, VolatilityOfConstantSeriesIsZero) {
    auto v =
        LiveHistoricalMetricsCalculator::calculate_annualized_volatility({0.5, 0.5, 0.5, 0.5});
    EXPECT_DOUBLE_EQ(v, 0.0);
}

// ===== calculate_annualized_downside_deviation =====

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationOfAllPositiveIsZero) {
    auto d = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        {1.0, 2.0, 3.0}, 0.0);
    EXPECT_DOUBLE_EQ(d, 0.0);
}

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationCountsBelowTargetOnly) {
    // Two negatives: -1, -2. Squares: 1, 4. Mean=2.5. Daily std = sqrt(2.5).
    // Annualized = sqrt(2.5) * sqrt(252).
    auto d = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        {1.0, -1.0, -2.0, 5.0}, 0.0);
    EXPECT_NEAR(d, std::sqrt(2.5) * std::sqrt(252.0), 1e-9);
}

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationFewerThanTwoNegativesIsZero) {
    auto d = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        {-1.0, 5.0, 5.0}, 0.0);
    EXPECT_DOUBLE_EQ(d, 0.0);
}

// ===== calculate_max_drawdown_from_equity =====

TEST_F(LiveHistoricalMetricsTest, MaxDrawdownOfEmptyIsZero) {
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_max_drawdown_from_equity({}), 0.0);
}

TEST_F(LiveHistoricalMetricsTest, MaxDrawdownOfMonotonicIncreasingIsZero) {
    EXPECT_DOUBLE_EQ(
        LiveHistoricalMetricsCalculator::calculate_max_drawdown_from_equity({100, 110, 120, 130}),
        0.0);
}

TEST_F(LiveHistoricalMetricsTest, MaxDrawdownComputesPercentLossFromPeak) {
    // Peak = 100, trough = 80. DD = 20%.
    auto dd =
        LiveHistoricalMetricsCalculator::calculate_max_drawdown_from_equity({100, 90, 80, 95});
    EXPECT_NEAR(dd, 20.0, 1e-9);
}

TEST_F(LiveHistoricalMetricsTest, MaxDrawdownTracksLargerDrawdown) {
    // Peak A=100, trough 90 → 10%. Then peak B=120, trough 96 → 20%.
    auto dd = LiveHistoricalMetricsCalculator::calculate_max_drawdown_from_equity(
        {100, 90, 100, 120, 96});
    EXPECT_NEAR(dd, 20.0, 1e-9);
}

// ===== calculate (full integration) =====

TEST_F(LiveHistoricalMetricsTest, CalculateEmptyReturnsZeroedMetricsExceptCounts) {
    LiveHistoricalMetricsCalculator c;
    auto m = c.calculate({}, {}, {}, 12.0, 5);
    EXPECT_EQ(m.total_days, 0);
    EXPECT_EQ(m.total_trades, 5);
    EXPECT_DOUBLE_EQ(m.sharpe_ratio, 0.0);
    EXPECT_DOUBLE_EQ(m.sortino_ratio, 0.0);
    EXPECT_DOUBLE_EQ(m.win_rate, 0.0);
}

TEST_F(LiveHistoricalMetricsTest, CalculatePopulatesAllAggregateStats) {
    LiveHistoricalMetricsCalculator c;
    std::vector<double> returns{1.0, -0.5, 2.0, -1.5, 0.5};
    std::vector<double> pnl{1000.0, -500.0, 2000.0, -1500.0, 500.0};
    std::vector<double> equity{100000, 101000, 100500, 102500, 101000, 101500};
    auto m = c.calculate(returns, pnl, equity, /*ann_return=*/15.0, /*trades=*/10);

    EXPECT_EQ(m.total_days, 5);
    EXPECT_EQ(m.total_trades, 10);
    EXPECT_EQ(m.winning_days, 3);
    EXPECT_EQ(m.losing_days, 2);
    EXPECT_NEAR(m.win_rate, 60.0, 1e-9);
    EXPECT_GT(m.volatility, 0.0);
    EXPECT_GT(m.sharpe_ratio, 0.0);
    EXPECT_GT(m.gross_profit, 0.0);
    EXPECT_GT(m.gross_loss, 0.0);
    EXPECT_NEAR(m.gross_profit, 3500.0, 1e-9);
    EXPECT_NEAR(m.gross_loss, 2000.0, 1e-9);
    EXPECT_NEAR(m.profit_factor, 1.75, 1e-9);
    EXPECT_DOUBLE_EQ(m.best_day, 2.0);
    EXPECT_DOUBLE_EQ(m.worst_day, -1.5);
}

TEST_F(LiveHistoricalMetricsTest, CalculateProfitFactorIsLargeWhenNoLosses) {
    LiveHistoricalMetricsCalculator c;
    auto m = c.calculate({1.0, 2.0}, {100.0, 200.0}, {1000.0, 1100.0}, 5.0, 2);
    EXPECT_GT(m.profit_factor, 100.0);
}

TEST_F(LiveHistoricalMetricsTest, CalculateZeroVolatilityKeepsRatiosAtZero) {
    LiveHistoricalMetricsCalculator c;
    auto m = c.calculate({0.5, 0.5, 0.5}, {}, {}, 10.0, 0);
    EXPECT_DOUBLE_EQ(m.volatility, 0.0);
    EXPECT_DOUBLE_EQ(m.sharpe_ratio, 0.0);
    EXPECT_DOUBLE_EQ(m.sortino_ratio, 0.0);
}
