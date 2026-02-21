#include <gtest/gtest.h>
#include "trade_ngin/statistics/tests/johansen_test.hpp"
#include <Eigen/Dense>
#include <random>
#include <limits>

using namespace trade_ngin::statistics;

class JohansenTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 100;
        // Create cointegrated series
        std::vector<double> z(n);
        z[0] = d(gen);
        for (int i = 1; i < n; ++i) {
            z[i] = z[i - 1] + d(gen);  // Random walk
        }

        // Create two series that share the random walk component
        cointegrated_data_ = Eigen::MatrixXd(n, 2);
        for (int i = 0; i < n; ++i) {
            cointegrated_data_(i, 0) = z[i] + 0.1 * d(gen);
            cointegrated_data_(i, 1) = 2.0 * z[i] + 0.1 * d(gen);
        }
    }

    Eigen::MatrixXd cointegrated_data_;
};

TEST_F(JohansenTestFixture, DetectsCointegration) {
    JohansenTestConfig config;
    JohansenTest johansen(config);

    auto result = johansen.test(cointegrated_data_);
    ASSERT_TRUE(result.is_ok());

    const auto& coint_result = result.value();
    EXPECT_EQ(coint_result.eigenvalues.size(), 2);
    EXPECT_EQ(coint_result.trace_statistics.size(), 2);
    // Should detect at least some cointegration
    EXPECT_GE(coint_result.cointegration_rank, 0);
}

TEST_F(JohansenTestFixture, InsufficientDataError) {
    JohansenTestConfig config;
    JohansenTest johansen(config);

    Eigen::MatrixXd small_data(5, 2);
    auto result = johansen.test(small_data);
    EXPECT_TRUE(result.is_error());
}

TEST(ValidationTests, JohansenNaNRejected) {
    Eigen::MatrixXd data(30, 2);
    data.setOnes();
    data(15, 0) = std::numeric_limits<double>::quiet_NaN();

    JohansenTestConfig config;
    JohansenTest johansen(config);
    auto result = johansen.test(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
