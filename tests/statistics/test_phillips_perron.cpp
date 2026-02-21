#include <gtest/gtest.h>
#include "trade_ngin/statistics/tests/phillips_perron_test.hpp"
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class PhillipsPerronTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Non-stationary random walk
        random_walk_.resize(200);
        random_walk_[0] = 0.0;
        for (size_t i = 1; i < random_walk_.size(); ++i) {
            random_walk_[i] = random_walk_[i - 1] + d(gen);
        }

        // Stationary white noise
        white_noise_.resize(200);
        for (size_t i = 0; i < white_noise_.size(); ++i) {
            white_noise_[i] = d(gen);
        }

        // Stationary AR(1) with autocorrelation
        ar1_.resize(200);
        ar1_[0] = 0.0;
        for (size_t i = 1; i < ar1_.size(); ++i) {
            ar1_[i] = 0.5 * ar1_[i - 1] + d(gen);
        }
    }

    std::vector<double> random_walk_;
    std::vector<double> white_noise_;
    std::vector<double> ar1_;
};

TEST_F(PhillipsPerronTestFixture, DetectsNonStationarity) {
    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);

    auto result = pp.test(random_walk_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_FALSE(res.reject_null);
    EXPECT_GT(res.statistic, res.critical_value);
}

TEST_F(PhillipsPerronTestFixture, DetectsStationarity) {
    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);

    auto result = pp.test(white_noise_);
    ASSERT_TRUE(result.is_ok());

    EXPECT_LT(result.value().statistic, 0.0);
}

TEST_F(PhillipsPerronTestFixture, RobustToSerialCorrelation) {
    // AR(1) is stationary but has serial correlation
    // PP should handle this better than naive t-test
    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);

    auto result = pp.test(ar1_);
    ASSERT_TRUE(result.is_ok());

    // AR(1) with phi=0.5 is stationary, PP should likely reject
    EXPECT_LT(result.value().statistic, 0.0);
}

TEST_F(PhillipsPerronTestFixture, WithTrend) {
    PhillipsPerronConfig config;
    config.regression = PhillipsPerronConfig::RegressionType::CONSTANT_TREND;
    PhillipsPerronTest pp(config);

    auto result = pp.test(random_walk_);
    ASSERT_TRUE(result.is_ok());

    // Should still detect non-stationarity
    EXPECT_FALSE(result.value().reject_null);
}

TEST_F(PhillipsPerronTestFixture, ParzenKernel) {
    PhillipsPerronConfig config;
    config.kernel = PhillipsPerronConfig::KernelType::PARZEN;
    PhillipsPerronTest pp(config);

    auto result = pp.test(white_noise_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_LT(result.value().statistic, 0.0);
}

TEST_F(PhillipsPerronTestFixture, ManualBandwidth) {
    PhillipsPerronConfig config;
    config.bandwidth = 5;
    PhillipsPerronTest pp(config);

    auto result = pp.test(white_noise_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().additional_stats.at("bandwidth"), 5.0);
}

TEST_F(PhillipsPerronTestFixture, AdditionalStats) {
    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);

    auto result = pp.test(white_noise_);
    ASSERT_TRUE(result.is_ok());

    const auto& stats = result.value().additional_stats;
    EXPECT_TRUE(stats.count("bandwidth") > 0);
    EXPECT_TRUE(stats.count("long_run_variance") > 0);
    EXPECT_TRUE(stats.count("short_run_variance") > 0);
    EXPECT_TRUE(stats.count("ols_t_stat") > 0);
}

TEST_F(PhillipsPerronTestFixture, InsufficientDataError) {
    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);

    std::vector<double> small = {1.0, 2.0, 3.0};
    auto result = pp.test(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(PhillipsPerronTestFixture, NaNRejected) {
    std::vector<double> data(100, 1.0);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);
    auto result = pp.test(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(PhillipsPerronTestFixture, GetName) {
    PhillipsPerronConfig config;
    PhillipsPerronTest pp(config);
    EXPECT_EQ(pp.get_name(), "Phillips-Perron Test");
}
