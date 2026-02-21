#include <gtest/gtest.h>
#include "trade_ngin/statistics/volatility/dcc_garch.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class DCCGARCHTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate bivariate returns with time-varying correlation
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int T = 300;
        returns_ = Eigen::MatrixXd(T, 2);

        double sigma1 = 0.01, sigma2 = 0.015;
        double rho = 0.5;

        for (int t = 0; t < T; ++t) {
            // Time-varying correlation
            rho = 0.3 + 0.4 * std::sin(2.0 * M_PI * t / 100.0);

            double z1 = d(gen);
            double z2 = rho * z1 + std::sqrt(1.0 - rho * rho) * d(gen);

            returns_(t, 0) = sigma1 * z1;
            returns_(t, 1) = sigma2 * z2;

            // Simple vol clustering
            sigma1 = std::sqrt(0.00001 + 0.1 * returns_(t, 0) * returns_(t, 0) + 0.85 * sigma1 * sigma1);
            sigma2 = std::sqrt(0.00001 + 0.1 * returns_(t, 1) * returns_(t, 1) + 0.85 * sigma2 * sigma2);
        }
    }

    Eigen::MatrixXd returns_;
};

TEST_F(DCCGARCHTest, FitBivariate) {
    DCCGARCHConfig config;
    DCCGARCH dcc(config);

    auto result = dcc.fit(returns_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(dcc.is_fitted());

    const auto& res = result.value();
    EXPECT_GT(res.dcc_a, 0.0);
    EXPECT_GT(res.dcc_b, 0.0);
    EXPECT_LT(res.dcc_a + res.dcc_b, 1.0);
}

TEST_F(DCCGARCHTest, CorrelationMatricesValid) {
    DCCGARCHConfig config;
    DCCGARCH dcc(config);

    auto result = dcc.fit(returns_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    int T = returns_.rows();

    for (int t = 0; t < T; ++t) {
        const auto& R = res.conditional_correlations[t];
        EXPECT_EQ(R.rows(), 2);
        EXPECT_EQ(R.cols(), 2);

        // Diagonal should be ~1
        EXPECT_NEAR(R(0, 0), 1.0, 0.01);
        EXPECT_NEAR(R(1, 1), 1.0, 0.01);

        // Off-diagonal should be in [-1, 1]
        EXPECT_GE(R(0, 1), -1.0);
        EXPECT_LE(R(0, 1), 1.0);

        // Symmetric
        EXPECT_NEAR(R(0, 1), R(1, 0), 1e-10);
    }
}

TEST_F(DCCGARCHTest, GetCorrelation) {
    DCCGARCHConfig config;
    DCCGARCH dcc(config);
    dcc.fit(returns_);

    auto r0 = dcc.get_correlation(0);
    ASSERT_TRUE(r0.is_ok());
    EXPECT_EQ(r0.value().rows(), 2);

    auto r_last = dcc.get_correlation(returns_.rows() - 1);
    ASSERT_TRUE(r_last.is_ok());
}

TEST_F(DCCGARCHTest, ForecastCorrelation) {
    DCCGARCHConfig config;
    DCCGARCH dcc(config);
    dcc.fit(returns_);

    auto forecast = dcc.forecast_correlation();
    ASSERT_TRUE(forecast.is_ok());

    const auto& R = forecast.value();
    EXPECT_EQ(R.rows(), 2);
    EXPECT_EQ(R.cols(), 2);
    EXPECT_NEAR(R(0, 0), 1.0, 0.01);
    EXPECT_NEAR(R(1, 1), 1.0, 0.01);
}

TEST_F(DCCGARCHTest, TimeVaryingCorrelation) {
    DCCGARCHConfig config;
    DCCGARCH dcc(config);

    auto result = dcc.fit(returns_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    // Correlations should vary over time
    double min_corr = 1.0, max_corr = -1.0;
    for (const auto& R : res.conditional_correlations) {
        double c = R(0, 1);
        min_corr = std::min(min_corr, c);
        max_corr = std::max(max_corr, c);
    }
    // Should have some variation
    EXPECT_GT(max_corr - min_corr, 0.01);
}

TEST_F(DCCGARCHTest, ThreeSeries) {
    std::mt19937 gen(123);
    std::normal_distribution<> d(0.0, 1.0);

    int T = 200;
    Eigen::MatrixXd returns(T, 3);
    for (int t = 0; t < T; ++t) {
        double z1 = d(gen);
        returns(t, 0) = 0.01 * z1;
        returns(t, 1) = 0.01 * (0.5 * z1 + 0.866 * d(gen));
        returns(t, 2) = 0.01 * (0.3 * z1 + 0.954 * d(gen));
    }

    DCCGARCHConfig config;
    DCCGARCH dcc(config);
    auto result = dcc.fit(returns);
    ASSERT_TRUE(result.is_ok());

    const auto& R = result.value().conditional_correlations[0];
    EXPECT_EQ(R.rows(), 3);
    EXPECT_EQ(R.cols(), 3);
}

TEST_F(DCCGARCHTest, InsufficientDataError) {
    Eigen::MatrixXd small(10, 2);
    small.setOnes();

    DCCGARCHConfig config;
    DCCGARCH dcc(config);
    auto result = dcc.fit(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(DCCGARCHTest, SingleSeriesError) {
    Eigen::MatrixXd single(100, 1);
    single.setOnes();

    DCCGARCHConfig config;
    DCCGARCH dcc(config);
    auto result = dcc.fit(single);
    EXPECT_TRUE(result.is_error());
}

TEST_F(DCCGARCHTest, NaNRejected) {
    Eigen::MatrixXd data(100, 2);
    data.setOnes();
    data(50, 0) = std::numeric_limits<double>::quiet_NaN();

    DCCGARCHConfig config;
    DCCGARCH dcc(config);
    auto result = dcc.fit(data);
    EXPECT_TRUE(result.is_error());
}

TEST_F(DCCGARCHTest, ForecastBeforeFitError) {
    DCCGARCHConfig config;
    DCCGARCH dcc(config);

    auto result = dcc.forecast_correlation();
    EXPECT_TRUE(result.is_error());
}
