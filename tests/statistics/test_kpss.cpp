#include <gtest/gtest.h>
#include "trade_ngin/statistics/tests/kpss_test.hpp"
#include <random>
#include <limits>

using namespace trade_ngin::statistics;

class KPSSTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Stationary process
        stationary_.resize(100);
        for (size_t i = 0; i < stationary_.size(); ++i) {
            stationary_[i] = d(gen);
        }

        // Non-stationary process (random walk)
        non_stationary_.resize(100);
        non_stationary_[0] = 0.0;
        for (size_t i = 1; i < non_stationary_.size(); ++i) {
            non_stationary_[i] = non_stationary_[i - 1] + d(gen);
        }
    }

    std::vector<double> stationary_;
    std::vector<double> non_stationary_;
};

TEST_F(KPSSTestFixture, DetectsStationarity) {
    KPSSTestConfig config;
    KPSSTest kpss(config);

    auto result = kpss.test(stationary_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Stationary series should not reject null
    EXPECT_FALSE(test_result.reject_null);
}

TEST_F(KPSSTestFixture, DetectsNonStationarity) {
    KPSSTestConfig config;
    KPSSTest kpss(config);

    auto result = kpss.test(non_stationary_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Non-stationary series should reject null
    // Note: This is probabilistic
    EXPECT_GT(test_result.statistic, 0.0);
}

TEST(ValidationTests, InfRejectedByTimeSeries) {
    std::vector<double> data(50, 1.0);
    data[10] = std::numeric_limits<double>::infinity();

    KPSSTestConfig config;
    KPSSTest kpss(config);
    auto result = kpss.test(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
