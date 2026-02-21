#include <gtest/gtest.h>
#include "trade_ngin/statistics/tests/variance_ratio_test.hpp"
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class VarianceRatioTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Random walk (price series): VR ≈ 1
        int n = 500;
        random_walk_.resize(n);
        random_walk_[0] = 100.0;
        for (int i = 1; i < n; ++i) {
            random_walk_[i] = random_walk_[i - 1] + d(gen);
        }

        // Mean-reverting series: VR < 1
        mean_reverting_.resize(n);
        mean_reverting_[0] = 100.0;
        for (int i = 1; i < n; ++i) {
            mean_reverting_[i] = 100.0 + 0.5 * (mean_reverting_[i - 1] - 100.0) + d(gen);
        }

        // Trending series (momentum): VR > 1
        trending_.resize(n);
        trending_[0] = 100.0;
        double inc = d(gen);
        for (int i = 1; i < n; ++i) {
            inc = 0.7 * inc + 0.3 * d(gen);
            trending_[i] = trending_[i - 1] + inc;
        }
    }

    std::vector<double> random_walk_;
    std::vector<double> mean_reverting_;
    std::vector<double> trending_;
};

TEST_F(VarianceRatioTestFixture, RandomWalkNotRejected) {
    VarianceRatioConfig config;
    config.holding_periods = {2, 5, 10};
    VarianceRatioTest vr(config);

    auto result = vr.test_multiple(random_walk_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_EQ(res.vr_statistics.size(), 3);
    EXPECT_EQ(res.z_statistics.size(), 3);
    EXPECT_EQ(res.p_values.size(), 3);

    // VR should be near 1 for random walk
    for (double vr_stat : res.vr_statistics) {
        EXPECT_NEAR(vr_stat, 1.0, 0.3);
    }
}

TEST_F(VarianceRatioTestFixture, MeanReversionDetected) {
    VarianceRatioConfig config;
    config.holding_periods = {2, 5, 10};
    config.heteroskedasticity_robust = false;
    VarianceRatioTest vr(config);

    auto result = vr.test_multiple(mean_reverting_);
    ASSERT_TRUE(result.is_ok());

    // Mean-reverting: VR < 1 (at least for some periods)
    bool any_below_one = false;
    for (double vr_stat : result.value().vr_statistics) {
        if (vr_stat < 1.0) any_below_one = true;
    }
    EXPECT_TRUE(any_below_one);
}

TEST_F(VarianceRatioTestFixture, TrendingDetected) {
    VarianceRatioConfig config;
    config.holding_periods = {2, 5, 10};
    config.heteroskedasticity_robust = false;
    VarianceRatioTest vr(config);

    auto result = vr.test_multiple(trending_);
    ASSERT_TRUE(result.is_ok());

    // Trending: VR > 1
    bool any_above_one = false;
    for (double vr_stat : result.value().vr_statistics) {
        if (vr_stat > 1.0) any_above_one = true;
    }
    EXPECT_TRUE(any_above_one);
}

TEST_F(VarianceRatioTestFixture, HeteroskedasticityRobust) {
    VarianceRatioConfig config;
    config.holding_periods = {2, 5};
    config.heteroskedasticity_robust = true;
    VarianceRatioTest vr(config);

    auto result = vr.test_multiple(random_walk_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().vr_statistics.size(), 2);
}

TEST_F(VarianceRatioTestFixture, SingleTestInterface) {
    VarianceRatioConfig config;
    config.holding_periods = {2, 5, 10};
    VarianceRatioTest vr(config);

    auto result = vr.test(random_walk_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_TRUE(res.additional_stats.count("q2_vr") > 0);
    EXPECT_TRUE(res.additional_stats.count("q5_vr") > 0);
}

TEST_F(VarianceRatioTestFixture, Interpretation) {
    VarianceRatioConfig config;
    config.holding_periods = {2, 5, 10};
    VarianceRatioTest vr(config);

    auto result = vr.test_multiple(random_walk_);
    ASSERT_TRUE(result.is_ok());

    EXPECT_FALSE(result.value().interpretation.empty());
}

TEST_F(VarianceRatioTestFixture, InsufficientDataError) {
    VarianceRatioConfig config;
    VarianceRatioTest vr(config);

    std::vector<double> small = {1.0, 2.0, 3.0};
    auto result = vr.test(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(VarianceRatioTestFixture, NaNRejected) {
    std::vector<double> data(100, 1.0);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    VarianceRatioConfig config;
    VarianceRatioTest vr(config);
    auto result = vr.test(data);
    EXPECT_TRUE(result.is_error());
}

TEST_F(VarianceRatioTestFixture, GetName) {
    VarianceRatioConfig config;
    VarianceRatioTest vr(config);
    EXPECT_EQ(vr.get_name(), "Variance Ratio Test");
}
