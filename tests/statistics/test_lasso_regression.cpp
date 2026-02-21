#include <gtest/gtest.h>
#include "trade_ngin/statistics/regression/lasso_regression.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class LassoRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // y = 2*x1 + 3*x2 + 0*x3 + 0*x4 + noise (sparse signal)
        std::mt19937 gen(42);
        std::normal_distribution<> noise(0.0, 0.3);
        std::normal_distribution<> feature(0.0, 1.0);

        int n = 200;
        X_ = Eigen::MatrixXd(n, 4);
        y_ = Eigen::VectorXd(n);

        for (int i = 0; i < n; ++i) {
            X_(i, 0) = feature(gen);
            X_(i, 1) = feature(gen);
            X_(i, 2) = feature(gen);
            X_(i, 3) = feature(gen);
            y_(i) = 2.0 * X_(i, 0) + 3.0 * X_(i, 1) + noise(gen);
        }
    }

    Eigen::MatrixXd X_;
    Eigen::VectorXd y_;
};

TEST_F(LassoRegressionTest, FeatureSelection) {
    LassoRegressionConfig config;
    config.alpha = 0.1;
    LassoRegression lasso(config);

    auto result = lasso.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    // First two features should have nonzero coefficients
    EXPECT_GT(std::abs(res.coefficients(0)), 0.5);
    EXPECT_GT(std::abs(res.coefficients(1)), 0.5);

    // With moderate alpha, irrelevant features should be smaller
    EXPECT_LT(std::abs(res.coefficients(2)), std::abs(res.coefficients(0)));
    EXPECT_LT(std::abs(res.coefficients(3)), std::abs(res.coefficients(1)));
}

TEST_F(LassoRegressionTest, AlphaZeroApproxOLS) {
    LassoRegressionConfig config;
    config.alpha = 1e-10;
    LassoRegression lasso(config);

    auto result = lasso.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    // All features should be nonzero with alpha ≈ 0
    EXPECT_EQ(result.value().n_nonzero, 4);
}

TEST_F(LassoRegressionTest, HighAlphaSparsity) {
    LassoRegressionConfig config;
    config.alpha = 10.0;
    LassoRegression lasso(config);

    auto result = lasso.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    // High alpha should drive many/all coefficients to zero
    EXPECT_LT(result.value().n_nonzero, 4);
}

TEST_F(LassoRegressionTest, Convergence) {
    LassoRegressionConfig config;
    config.alpha = 0.05;
    config.max_iterations = 5000;
    config.tolerance = 1e-8;
    LassoRegression lasso(config);

    auto result = lasso.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_GT(result.value().r_squared, 0.8);
}

TEST_F(LassoRegressionTest, CrossValidation) {
    LassoRegressionConfig config;
    config.cv_folds = 5;
    config.alpha_candidates = {0.001, 0.01, 0.1, 1.0, 5.0};

    LassoRegression lasso(config);
    auto result = lasso.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_GT(res.best_alpha, 0.0);
    EXPECT_GT(res.r_squared, 0.5);
}

TEST_F(LassoRegressionTest, SelectedFeatures) {
    LassoRegressionConfig config;
    config.alpha = 0.5;
    LassoRegression lasso(config);

    auto result = lasso.fit(X_, y_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_EQ(static_cast<int>(res.selected_features.size()), res.n_nonzero);

    for (int idx : res.selected_features) {
        EXPECT_GE(idx, 0);
        EXPECT_LT(idx, 4);
    }
}

TEST_F(LassoRegressionTest, Predict) {
    LassoRegressionConfig config;
    config.alpha = 0.1;
    LassoRegression lasso(config);
    lasso.fit(X_, y_);

    Eigen::MatrixXd X_new(2, 4);
    X_new.setZero();
    X_new(0, 0) = 1.0;
    X_new(1, 1) = 1.0;

    auto pred = lasso.predict(X_new);
    ASSERT_TRUE(pred.is_ok());
    EXPECT_EQ(pred.value().size(), 2);
}

TEST_F(LassoRegressionTest, DimensionMismatchError) {
    LassoRegression lasso;
    Eigen::VectorXd y_bad(5);
    y_bad.setZero();

    auto result = lasso.fit(X_, y_bad);
    EXPECT_TRUE(result.is_error());
}

TEST_F(LassoRegressionTest, NaNRejected) {
    Eigen::MatrixXd X(10, 2);
    X.setOnes();

    Eigen::VectorXd y(10);
    y.setOnes();
    y(5) = std::numeric_limits<double>::quiet_NaN();

    LassoRegression lasso;
    auto result = lasso.fit(X, y);
    EXPECT_TRUE(result.is_error());
}

TEST_F(LassoRegressionTest, PredictBeforeFitError) {
    LassoRegression lasso;
    Eigen::MatrixXd X_new(1, 4);
    X_new.setZero();

    auto result = lasso.predict(X_new);
    EXPECT_TRUE(result.is_error());
}
