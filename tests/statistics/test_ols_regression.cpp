#include <gtest/gtest.h>
#include "trade_ngin/statistics/regression/ols_regression.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class OLSRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Simple linear: y = 2 + 3*x + noise
        std::mt19937 gen(42);
        std::normal_distribution<> noise(0.0, 0.1);

        int n = 100;
        X_ = Eigen::MatrixXd(n, 1);
        y_ = Eigen::VectorXd(n);

        for (int i = 0; i < n; ++i) {
            X_(i, 0) = static_cast<double>(i) / n;
            y_(i) = 2.0 + 3.0 * X_(i, 0) + noise(gen);
        }
    }

    Eigen::MatrixXd X_;
    Eigen::VectorXd y_;
};

TEST_F(OLSRegressionTest, SimpleRegression) {
    OLSRegression ols;
    auto result = ols.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    // Intercept ≈ 2.0, slope ≈ 3.0
    EXPECT_NEAR(res.coefficients(0), 2.0, 0.5);
    EXPECT_NEAR(res.coefficients(1), 3.0, 0.5);
    EXPECT_GT(res.r_squared, 0.9);
    EXPECT_GT(res.adj_r_squared, 0.9);
}

TEST_F(OLSRegressionTest, MultipleRegression) {
    // y = 1 + 2*x1 + 3*x2
    std::mt19937 gen(42);
    std::normal_distribution<> noise(0.0, 0.3);

    int n = 100;
    Eigen::MatrixXd X(n, 2);
    Eigen::VectorXd y(n);

    for (int i = 0; i < n; ++i) {
        X(i, 0) = static_cast<double>(i) / n;
        X(i, 1) = static_cast<double>(i * i) / (n * n);
        y(i) = 1.0 + 2.0 * X(i, 0) + 3.0 * X(i, 1) + noise(gen);
    }

    OLSRegression ols;
    auto result = ols.fit(X, y);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_NEAR(res.coefficients(0), 1.0, 0.5);
    EXPECT_NEAR(res.coefficients(1), 2.0, 1.0);
    EXPECT_NEAR(res.coefficients(2), 3.0, 1.0);
    EXPECT_GT(res.r_squared, 0.95);
}

TEST_F(OLSRegressionTest, NoIntercept) {
    OLSRegressionConfig config;
    config.include_intercept = false;

    // y = 3*x (no intercept)
    int n = 50;
    Eigen::MatrixXd X(n, 1);
    Eigen::VectorXd y(n);

    for (int i = 0; i < n; ++i) {
        X(i, 0) = static_cast<double>(i + 1) / n;
        y(i) = 3.0 * X(i, 0);
    }

    OLSRegression ols(config);
    auto result = ols.fit(X, y);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_EQ(res.coefficients.size(), 1);
    EXPECT_NEAR(res.coefficients(0), 3.0, 0.01);
}

TEST_F(OLSRegressionTest, PerfectFit) {
    // y = 1 + 2*x exactly (no noise)
    int n = 50;
    Eigen::MatrixXd X(n, 1);
    Eigen::VectorXd y(n);
    for (int i = 0; i < n; ++i) {
        X(i, 0) = static_cast<double>(i);
        y(i) = 1.0 + 2.0 * X(i, 0);
    }

    OLSRegression ols;
    auto result = ols.fit(X, y);
    ASSERT_TRUE(result.is_ok());

    EXPECT_NEAR(result.value().r_squared, 1.0, 1e-10);
    EXPECT_NEAR(result.value().coefficients(0), 1.0, 1e-10);
    EXPECT_NEAR(result.value().coefficients(1), 2.0, 1e-10);
}

TEST_F(OLSRegressionTest, Predict) {
    OLSRegression ols;
    ols.fit(X_, y_);

    Eigen::MatrixXd X_new(3, 1);
    X_new << 0.0, 0.5, 1.0;

    auto pred_result = ols.predict(X_new);
    ASSERT_TRUE(pred_result.is_ok());
    EXPECT_EQ(pred_result.value().size(), 3);
}

TEST_F(OLSRegressionTest, Diagnostics) {
    OLSRegression ols;
    auto result = ols.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_EQ(res.residuals.size(), X_.rows());
    EXPECT_EQ(res.standard_errors.size(), res.coefficients.size());
    EXPECT_EQ(res.t_statistics.size(), res.coefficients.size());
    EXPECT_EQ(res.p_values.size(), res.coefficients.size());

    // All SEs should be positive
    for (int i = 0; i < res.standard_errors.size(); ++i) {
        EXPECT_GT(res.standard_errors(i), 0.0);
    }

    // Slope p-value should be very small (significant)
    EXPECT_LT(res.p_values(1), 0.05);
}

TEST_F(OLSRegressionTest, DimensionMismatchError) {
    OLSRegression ols;
    Eigen::VectorXd y_short(5);
    y_short.setZero();

    auto result = ols.fit(X_, y_short);
    EXPECT_TRUE(result.is_error());
}

TEST_F(OLSRegressionTest, NaNRejected) {
    Eigen::MatrixXd X(10, 1);
    X.setOnes();
    X(5, 0) = std::numeric_limits<double>::quiet_NaN();

    Eigen::VectorXd y(10);
    y.setOnes();

    OLSRegression ols;
    auto result = ols.fit(X, y);
    EXPECT_TRUE(result.is_error());
}

TEST_F(OLSRegressionTest, PredictBeforeFitError) {
    OLSRegression ols;
    Eigen::MatrixXd X_new(1, 1);
    X_new << 1.0;

    auto result = ols.predict(X_new);
    EXPECT_TRUE(result.is_error());
}
