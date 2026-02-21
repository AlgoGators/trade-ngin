#include <gtest/gtest.h>
#include "trade_ngin/statistics/hurst_exponent.hpp"
#include <random>
#include <cmath>
#include <limits>
#include <numeric>

using namespace trade_ngin::statistics;

class HurstExponentTest : public ::testing::Test {
protected:
    void SetUp() override {
        // IID noise: H ≈ 0.5 (DFA/R/S operate on increments, not cumulated series)
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 2000;
        iid_noise_.resize(n);
        for (int i = 0; i < n; ++i) {
            iid_noise_[i] = d(gen);
        }

        // Trending: positively correlated increments (momentum)
        trending_.resize(n);
        trending_[0] = d(gen);
        for (int i = 1; i < n; ++i) {
            trending_[i] = 0.7 * trending_[i - 1] + 0.3 * d(gen);
        }
        // Cumulate to get persistent price series
        for (int i = 1; i < n; ++i) {
            trending_[i] += trending_[i - 1];
        }

        // Mean-reverting: anti-correlated increments
        mean_reverting_.resize(n);
        mean_reverting_[0] = 0.0;
        for (int i = 1; i < n; ++i) {
            mean_reverting_[i] = -0.5 * mean_reverting_[i - 1] + d(gen);
        }
    }

    std::vector<double> iid_noise_;
    std::vector<double> trending_;
    std::vector<double> mean_reverting_;
};

TEST_F(HurstExponentTest, RandomWalkDFA) {
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::DFA;
    HurstExponent hurst(config);

    auto result = hurst.compute(iid_noise_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    // DFA on iid noise gives H ≈ 0.5
    EXPECT_NEAR(res.hurst_exponent, 0.5, 0.2);
}

TEST_F(HurstExponentTest, TrendingDFA) {
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::DFA;
    HurstExponent hurst(config);

    auto result = hurst.compute(trending_);
    ASSERT_TRUE(result.is_ok());

    EXPECT_GT(result.value().hurst_exponent, 0.55);
}

TEST_F(HurstExponentTest, MeanRevertingDFA) {
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::DFA;
    HurstExponent hurst(config);

    auto result = hurst.compute(mean_reverting_);
    ASSERT_TRUE(result.is_ok());

    EXPECT_LT(result.value().hurst_exponent, 0.45);
}

TEST_F(HurstExponentTest, RSAnalysis) {
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::RS_ANALYSIS;
    HurstExponent hurst(config);

    auto result = hurst.compute(iid_noise_);
    ASSERT_TRUE(result.is_ok());

    // R/S on iid noise gives H ≈ 0.5
    EXPECT_NEAR(result.value().hurst_exponent, 0.5, 0.2);
}

TEST_F(HurstExponentTest, Periodogram) {
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::PERIODOGRAM;
    HurstExponent hurst(config);

    auto result = hurst.compute(iid_noise_);
    ASSERT_TRUE(result.is_ok());

    // Periodogram on iid noise: H should be near 0.5
    EXPECT_GT(result.value().hurst_exponent, 0.1);
    EXPECT_LT(result.value().hurst_exponent, 0.9);
}

TEST_F(HurstExponentTest, RSquaredReasonable) {
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::DFA;
    HurstExponent hurst(config);

    auto result = hurst.compute(iid_noise_);
    ASSERT_TRUE(result.is_ok());

    // Log-log regression should have decent fit
    EXPECT_GT(result.value().r_squared, 0.8);
}

TEST_F(HurstExponentTest, InsufficientDataError) {
    HurstExponent hurst;
    std::vector<double> small = {1.0, 2.0, 3.0};

    auto result = hurst.compute(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(HurstExponentTest, NaNRejected) {
    std::vector<double> data(100, 1.0);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    HurstExponent hurst;
    auto result = hurst.compute(data);
    EXPECT_TRUE(result.is_error());
}

TEST_F(HurstExponentTest, InterpretationStrings) {
    // Verify interpretation strings are correctly set
    HurstExponentConfig config;
    config.method = HurstExponentConfig::Method::DFA;
    HurstExponent hurst(config);

    auto result = hurst.compute(trending_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().interpretation == "trending" ||
                result.value().interpretation == "random walk" ||
                result.value().interpretation == "mean-reverting");
}
