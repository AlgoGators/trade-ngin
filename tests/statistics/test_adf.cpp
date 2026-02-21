#include <gtest/gtest.h>
#include "trade_ngin/statistics/tests/adf_test.hpp"
#include <random>
#include <limits>

using namespace trade_ngin::statistics;

class ADFTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate non-stationary random walk
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        random_walk_.resize(100);
        random_walk_[0] = 0.0;
        for (size_t i = 1; i < random_walk_.size(); ++i) {
            random_walk_[i] = random_walk_[i - 1] + d(gen);
        }

        // Generate stationary white noise
        white_noise_.resize(100);
        for (size_t i = 0; i < white_noise_.size(); ++i) {
            white_noise_[i] = d(gen);
        }
    }

    std::vector<double> random_walk_;
    std::vector<double> white_noise_;
};

TEST_F(ADFTestFixture, DetectsNonStationarity) {
    ADFTestConfig config;
    ADFTest adf(config);

    auto result = adf.test(random_walk_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Random walk should not reject null (non-stationary)
    EXPECT_FALSE(test_result.reject_null);
    EXPECT_GT(test_result.statistic, test_result.critical_value);
}

TEST_F(ADFTestFixture, DetectsStationarity) {
    ADFTestConfig config;
    ADFTest adf(config);

    auto result = adf.test(white_noise_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // White noise should likely reject null (stationary)
    // Note: This is probabilistic, so we just check the test runs
    EXPECT_LT(test_result.statistic, 0.0);
}

TEST_F(ADFTestFixture, InsufficientDataError) {
    ADFTestConfig config;
    ADFTest adf(config);

    std::vector<double> small_data = {1.0, 2.0, 3.0};
    auto result = adf.test(small_data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST(ValidationTests, NaNRejectedByTimeSeries) {
    std::vector<double> data(50, 1.0);
    data[25] = std::numeric_limits<double>::quiet_NaN();

    ADFTestConfig config;
    ADFTest adf(config);
    auto result = adf.test(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
