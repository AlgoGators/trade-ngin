#include <gtest/gtest.h>
#include "trade_ngin/statistics/tests/engle_granger_test.hpp"
#include <random>
#include <limits>

using namespace trade_ngin::statistics;

class EngleGrangerTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 100;
        // Create cointegrated pair
        y_.resize(n);
        x_.resize(n);

        std::vector<double> common_trend(n);
        common_trend[0] = d(gen);
        for (int i = 1; i < n; ++i) {
            common_trend[i] = common_trend[i - 1] + d(gen);
        }

        for (int i = 0; i < n; ++i) {
            x_[i] = common_trend[i] + 0.1 * d(gen);
            y_[i] = 2.0 * common_trend[i] + 1.0 + 0.1 * d(gen);
        }
    }

    std::vector<double> y_;
    std::vector<double> x_;
};

TEST_F(EngleGrangerTestFixture, DetectsCointegration) {
    EngleGrangerConfig config;
    EngleGrangerTest eg(config);

    auto result = eg.test(y_, x_);
    ASSERT_TRUE(result.is_ok());

    const auto& test_result = result.value();
    // Should find cointegrating relationship
    EXPECT_NE(test_result.additional_stats.count("regression_coefficient"), 0);
    EXPECT_NEAR(test_result.additional_stats.at("regression_coefficient"), 2.0, 0.5);
}

TEST_F(EngleGrangerTestFixture, MismatchedLengthError) {
    EngleGrangerConfig config;
    EngleGrangerTest eg(config);

    std::vector<double> short_x = {1.0, 2.0};
    auto result = eg.test(y_, short_x);
    EXPECT_TRUE(result.is_error());
}

TEST(ValidationTests, EngleGrangerNaNRejected) {
    std::vector<double> y(50, 1.0);
    std::vector<double> x(50, 2.0);
    y[10] = std::numeric_limits<double>::quiet_NaN();

    EngleGrangerConfig config;
    EngleGrangerTest eg(config);
    auto result = eg.test(y, x);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
