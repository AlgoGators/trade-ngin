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

namespace {
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
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_annualized_volatility({}, 252.0), 0.0);
    EXPECT_DOUBLE_EQ(LiveHistoricalMetricsCalculator::calculate_annualized_volatility({1.0}, 252.0), 0.0);
}

TEST_F(LiveHistoricalMetricsTest, VolatilityIsTheSampleDeviationTimesRootOfTheSessionsAYear) {
    // Returns 1.0 and -1.0: mean 0, squares 1 + 1 = 2 over n - 1 = 1, sample sd = sqrt(2).
    // On the futures grid: sqrt(2) x sqrt(311.0574) = 24.942229...; on NYSE sessions sqrt(504).
    auto v =
        LiveHistoricalMetricsCalculator::calculate_annualized_volatility({1.0, -1.0}, 311.0574);
    EXPECT_NEAR(v, std::sqrt(2.0) * std::sqrt(311.0574), 1e-9);
    EXPECT_NEAR(v, 24.942229, 1e-6);
    EXPECT_NEAR(LiveHistoricalMetricsCalculator::calculate_annualized_volatility({1.0, -1.0}, 252.0),
                std::sqrt(504.0), 1e-9);
    // {1, -2, 3, 0, -1}: mean 0.2, squares 14.8, over 4 = 3.7 (a population divisor gives 2.96).
    EXPECT_NEAR(LiveHistoricalMetricsCalculator::calculate_annualized_volatility(
                    {1.0, -2.0, 3.0, 0.0, -1.0}, 252.0),
                std::sqrt(3.7) * std::sqrt(252.0), 1e-9);
}

TEST_F(LiveHistoricalMetricsTest, VolatilityOfConstantSeriesIsZero) {
    auto v =
        LiveHistoricalMetricsCalculator::calculate_annualized_volatility({0.5, 0.5, 0.5, 0.5},
                                                                         252.0);
    EXPECT_DOUBLE_EQ(v, 0.0);
}

// ===== calculate_annualized_downside_deviation =====

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationOfAllPositiveIsZero) {
    auto d = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        {1.0, 2.0, 3.0}, 252.0, 0.0);
    EXPECT_DOUBLE_EQ(d, 0.0);
}

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationCountsBelowTargetOnly) {
    // Two negatives: -1, -2. Squares: 1, 4, summed 5 and averaged over ALL FOUR days
    // (the two days above target count as zeros): 5/4 = 1.25. Daily = sqrt(1.25).
    // Annualized = sqrt(1.25) * sqrt(252) = 17.748239349.
    auto d = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        {1.0, -1.0, -2.0, 5.0}, 252.0, 0.0);
    EXPECT_NEAR(d, 17.748239349, 1e-9);
    EXPECT_NEAR(d, std::sqrt(5.0 / 4.0) * std::sqrt(252.0), 1e-12);
    // The same series on the futures grid's sessions a year.
    EXPECT_NEAR(LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
                    {1.0, -1.0, -2.0, 5.0}, 311.0574, 0.0),
                std::sqrt(5.0 / 4.0) * std::sqrt(311.0574), 1e-12);
}

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationOfOneLosingDayIsItsShareOfTheSeries) {
    // One negative in three days: 1/3. Annualized = sqrt(1/3) * sqrt(252) = 9.165151390.
    // A single losing day is a downside, not a zero.
    auto d = LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation(
        {-1.0, 5.0, 5.0}, 252.0, 0.0);
    EXPECT_NEAR(d, 9.165151390, 1e-9);
}

TEST_F(LiveHistoricalMetricsTest, DownsideDeviationOfFewerThanTwoReturnsIsZero) {
    EXPECT_DOUBLE_EQ(
        LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation({}, 252.0,
                                                                                0.0),
        0.0);
    EXPECT_DOUBLE_EQ(
        LiveHistoricalMetricsCalculator::calculate_annualized_downside_deviation({-1.0}, 252.0,
                                                                                0.0),
        0.0);
}

TEST_F(LiveHistoricalMetricsTest, CalculateSortinoOfOneLosingDayDividesByItsDownside) {
    // Grid returns in percent {-1, 5, 5}, K 252: downside = sqrt(1/3) * sqrt(252) = 9.165151390;
    // Sortino = mean x K / that = 3 x 252 / 9.165151390 = 82.486363.
    LiveHistoricalMetricsCalculator c;
    auto m = c.calculate({}, {}, {}, 0, grid_of_returns({-1.0, 5.0, 5.0}), 252.0);
    EXPECT_NEAR(m.downside_deviation, 9.165151390, 1e-9);
    EXPECT_NEAR(m.sortino_ratio, 3.0 * 252.0 / 9.165151390, 1e-6);
    EXPECT_NEAR(m.sortino_ratio, 82.486363, 1e-5);
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
    auto m = c.calculate({}, {}, {}, 5, flat_grid(0), 252.0);
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
    auto m = c.calculate(returns, pnl, equity, /*trades=*/10, grid_of_returns(returns), 252.0);

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
    auto m = c.calculate({1.0, 2.0}, {100.0, 200.0}, {1000.0, 1100.0}, 2, flat_grid(2), 252.0);
    EXPECT_GT(m.profit_factor, 100.0);
}

TEST_F(LiveHistoricalMetricsTest, CalculateZeroVolatilityKeepsRatiosAtZero) {
    LiveHistoricalMetricsCalculator c;
    auto m = c.calculate({0.5, 0.5, 0.5}, {}, {}, 0, flat_grid(3), 252.0);
    EXPECT_DOUBLE_EQ(m.volatility, 0.0);
    EXPECT_DOUBLE_EQ(m.sharpe_ratio, 0.0);
    EXPECT_DOUBLE_EQ(m.sortino_ratio, 0.0);
}
