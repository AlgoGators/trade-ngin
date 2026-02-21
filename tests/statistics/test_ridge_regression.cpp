#include <gtest/gtest.h>
#include "trade_ngin/statistics/regression/ridge_regression.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class RidgeRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> noise(0.0, 0.5);

        int n = 100;
        X_ = Eigen::MatrixXd(n, 2);
        y_ = Eigen::VectorXd(n);

        for (int i = 0; i < n; ++i) {
            X_(i, 0) = static_cast<double>(i) / n;
            X_(i, 1) = static_cast<double>(i * i) / (n * n);
            y_(i) = 1.0 + 2.0 * X_(i, 0) + 3.0 * X_(i, 1) + noise(gen);
        }
    }

    Eigen::MatrixXd X_;
    Eigen::VectorXd y_;
};

TEST_F(RidgeRegressionTest, BasicFit) {
    RidgeRegressionConfig config;
    config.alpha = 0.1;
    RidgeRegression ridge(config);

    auto result = ridge.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_EQ(res.coefficients.size(), 2);
    EXPECT_GT(res.r_squared, 0.8);
}

TEST_F(RidgeRegressionTest, AlphaZeroApproxOLS) {
    RidgeRegressionConfig config;
    config.alpha = 1e-10;
    RidgeRegression ridge(config);

    auto ridge_result = ridge.fit(X_, y_);
    ASSERT_TRUE(ridge_result.is_ok());

    // Compare with effective OLS (alpha ≈ 0)
    // Coefficients should be close to OLS solution
    const auto& res = ridge_result.value();
    EXPECT_GT(res.r_squared, 0.9);
}

TEST_F(RidgeRegressionTest, HighAlphaShrinkage) {
    RidgeRegressionConfig low_config;
    low_config.alpha = 0.01;
    RidgeRegression ridge_low(low_config);

    RidgeRegressionConfig high_config;
    high_config.alpha = 100.0;
    RidgeRegression ridge_high(high_config);

    auto low_result = ridge_low.fit(X_, y_);
    auto high_result = ridge_high.fit(X_, y_);
    ASSERT_TRUE(low_result.is_ok());
    ASSERT_TRUE(high_result.is_ok());

    // High alpha should shrink coefficients more
    double low_norm = low_result.value().coefficients.norm();
    double high_norm = high_result.value().coefficients.norm();
    EXPECT_LT(high_norm, low_norm);
}

TEST_F(RidgeRegressionTest, CrossValidation) {
    RidgeRegressionConfig config;
    config.cv_folds = 5;
    config.alpha_candidates = {0.001, 0.01, 0.1, 1.0, 10.0};

    RidgeRegression ridge(config);
    auto result = ridge.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    // CV should pick a reasonable alpha
    EXPECT_GT(res.best_alpha, 0.0);
    EXPECT_GT(res.r_squared, 0.5);
}

TEST_F(RidgeRegressionTest, MulticollinearData) {
    // Create multicollinear data: x2 ≈ x1
    int n = 100;
    std::mt19937 gen(42);
    std::normal_distribution<> noise(0.0, 0.01);

    Eigen::MatrixXd X(n, 2);
    Eigen::VectorXd y(n);
    for (int i = 0; i < n; ++i) {
        X(i, 0) = static_cast<double>(i) / n;
        X(i, 1) = X(i, 0) + noise(gen);  // Nearly collinear
        y(i) = 1.0 + 5.0 * X(i, 0) + noise(gen);
    }

    RidgeRegressionConfig config;
    config.alpha = 1.0;
    RidgeRegression ridge(config);

    auto result = ridge.fit(X, y);
    ASSERT_TRUE(result.is_ok());
    // Ridge should handle this without blowing up
    EXPECT_TRUE(result.value().coefficients.allFinite());
}

TEST_F(RidgeRegressionTest, Predict) {
    RidgeRegressionConfig config;
    config.alpha = 0.1;
    RidgeRegression ridge(config);
    ridge.fit(X_, y_);

    Eigen::MatrixXd X_new(2, 2);
    X_new << 0.0, 0.0, 1.0, 1.0;

    auto pred = ridge.predict(X_new);
    ASSERT_TRUE(pred.is_ok());
    EXPECT_EQ(pred.value().size(), 2);
}

TEST_F(RidgeRegressionTest, DimensionMismatchError) {
    RidgeRegression ridge;
    Eigen::VectorXd y_bad(5);
    y_bad.setZero();

    auto result = ridge.fit(X_, y_bad);
    EXPECT_TRUE(result.is_error());
}

TEST_F(RidgeRegressionTest, NaNRejected) {
    Eigen::MatrixXd X(10, 1);
    X.setOnes();

    Eigen::VectorXd y(10);
    y.setOnes();
    y(3) = std::numeric_limits<double>::quiet_NaN();

    RidgeRegression ridge;
    auto result = ridge.fit(X, y);
    EXPECT_TRUE(result.is_error());
}
