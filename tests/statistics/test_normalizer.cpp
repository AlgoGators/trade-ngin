#include <gtest/gtest.h>
#include "trade_ngin/statistics/transformers/normalizer.hpp"
#include <Eigen/Dense>
#include <limits>

using namespace trade_ngin::statistics;

class NormalizerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test data
        data_ = Eigen::MatrixXd(5, 3);
        data_ << 1.0, 2.0, 3.0,
                 2.0, 4.0, 6.0,
                 3.0, 6.0, 9.0,
                 4.0, 8.0, 12.0,
                 5.0, 10.0, 15.0;
    }

    Eigen::MatrixXd data_;
};

TEST_F(NormalizerTest, ZScoreNormalization) {
    NormalizationConfig config;
    config.method = NormalizationConfig::Method::Z_SCORE;

    Normalizer normalizer(config);

    auto fit_result = normalizer.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(normalizer.is_fitted());

    auto transform_result = normalizer.transform(data_);
    ASSERT_TRUE(transform_result.is_ok());

    const auto& transformed = transform_result.value();

    // Check that mean is approximately 0
    Eigen::VectorXd col_means = transformed.colwise().mean();
    for (int i = 0; i < col_means.size(); ++i) {
        EXPECT_NEAR(col_means(i), 0.0, 1e-10);
    }

    // Check that std is approximately 1
    Eigen::VectorXd col_std = ((transformed.rowwise() - col_means.transpose()).array().square()
                               .colwise().sum() / (transformed.rows() - 1)).sqrt();
    for (int i = 0; i < col_std.size(); ++i) {
        EXPECT_NEAR(col_std(i), 1.0, 1e-10);
    }
}

TEST_F(NormalizerTest, MinMaxNormalization) {
    NormalizationConfig config;
    config.method = NormalizationConfig::Method::MIN_MAX;

    Normalizer normalizer(config);

    auto fit_result = normalizer.fit(data_);
    EXPECT_TRUE(fit_result.is_ok());

    auto transform_result = normalizer.transform(data_);
    ASSERT_TRUE(transform_result.is_ok());

    const auto& transformed = transform_result.value();

    // Check that min is 0 and max is 1
    Eigen::VectorXd col_min = transformed.colwise().minCoeff();
    Eigen::VectorXd col_max = transformed.colwise().maxCoeff();

    for (int i = 0; i < col_min.size(); ++i) {
        EXPECT_NEAR(col_min(i), 0.0, 1e-10);
        EXPECT_NEAR(col_max(i), 1.0, 1e-10);
    }
}

TEST_F(NormalizerTest, InverseTransform) {
    NormalizationConfig config;
    config.method = NormalizationConfig::Method::Z_SCORE;

    Normalizer normalizer(config);
    normalizer.fit(data_);

    auto transformed = normalizer.transform(data_);
    ASSERT_TRUE(transformed.is_ok());

    auto inverse_result = normalizer.inverse_transform(transformed.value());
    ASSERT_TRUE(inverse_result.is_ok());

    const auto& reconstructed = inverse_result.value();

    // Check that we get back original data
    for (int i = 0; i < data_.rows(); ++i) {
        for (int j = 0; j < data_.cols(); ++j) {
            EXPECT_NEAR(reconstructed(i, j), data_(i, j), 1e-10);
        }
    }
}

TEST_F(NormalizerTest, NotFittedError) {
    NormalizationConfig config;
    Normalizer normalizer(config);

    auto transform_result = normalizer.transform(data_);
    EXPECT_TRUE(transform_result.is_error());
    EXPECT_EQ(transform_result.error()->code(), trade_ngin::ErrorCode::NOT_INITIALIZED);
}

TEST(ValidationTests, NaNRejectedByMatrix) {
    Eigen::MatrixXd data(5, 3);
    data.setOnes();
    data(2, 1) = std::numeric_limits<double>::quiet_NaN();

    NormalizationConfig config;
    Normalizer normalizer(config);
    auto result = normalizer.fit(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
