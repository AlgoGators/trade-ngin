#include <gtest/gtest.h>
#include "trade_ngin/statistics/transformers/pca.hpp"
#include <Eigen/Dense>
#include <random>
#include <limits>

using namespace trade_ngin::statistics;

class PCATest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create correlated test data
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 100;
        data_ = Eigen::MatrixXd(n, 3);

        for (int i = 0; i < n; ++i) {
            double x1 = d(gen);
            double x2 = 0.8 * x1 + 0.2 * d(gen);  // Correlated with x1
            double x3 = d(gen);  // Independent
            data_(i, 0) = x1;
            data_(i, 1) = x2;
            data_(i, 2) = x3;
        }
    }

    Eigen::MatrixXd data_;
};

TEST_F(PCATest, BasicFitTransform) {
    PCAConfig config;
    config.n_components = 2;

    PCA pca(config);

    auto fit_result = pca.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(pca.is_fitted());
    EXPECT_EQ(pca.get_n_components(), 2);

    auto transform_result = pca.transform(data_);
    ASSERT_TRUE(transform_result.is_ok());

    const auto& transformed = transform_result.value();
    EXPECT_EQ(transformed.cols(), 2);
    EXPECT_EQ(transformed.rows(), data_.rows());
}

TEST_F(PCATest, VarianceThreshold) {
    PCAConfig config;
    config.n_components = -1;  // Use variance threshold
    config.variance_threshold = 0.95;

    PCA pca(config);
    auto fit_result = pca.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());

    // Should select components that explain 95% of variance
    const auto& var_ratio = pca.get_explained_variance_ratio();
    double cumsum = 0.0;
    for (int i = 0; i < var_ratio.size(); ++i) {
        cumsum += var_ratio(i);
    }
    EXPECT_GE(cumsum, 0.95);
}

TEST_F(PCATest, InverseTransform) {
    PCAConfig config;
    config.n_components = 3;  // Keep all components

    PCA pca(config);
    pca.fit(data_);

    auto transformed = pca.transform(data_);
    ASSERT_TRUE(transformed.is_ok());

    auto inverse_result = pca.inverse_transform(transformed.value());
    ASSERT_TRUE(inverse_result.is_ok());

    const auto& reconstructed = inverse_result.value();

    // Should reconstruct original data well
    for (int i = 0; i < data_.rows(); ++i) {
        for (int j = 0; j < data_.cols(); ++j) {
            EXPECT_NEAR(reconstructed(i, j), data_(i, j), 0.1);
        }
    }
}

TEST(ValidationTests, EmptyMatrixRejected) {
    Eigen::MatrixXd empty(0, 0);

    PCAConfig config;
    PCA pca(config);
    auto result = pca.fit(empty);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}
