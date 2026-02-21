#include <gtest/gtest.h>
#include "trade_ngin/statistics/statistics.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>

using namespace trade_ngin::statistics;

class IntegrationTest : public ::testing::Test {};

TEST_F(IntegrationTest, NormalizationBeforePCA) {
    // Generate test data
    Eigen::MatrixXd data(50, 3);
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);

    for (int i = 0; i < 50; ++i) {
        data(i, 0) = d(gen) * 10.0 + 100.0;  // Different scales
        data(i, 1) = d(gen) * 0.1 + 1.0;
        data(i, 2) = d(gen) * 1.0 + 10.0;
    }

    // Normalize first
    NormalizationConfig norm_config;
    norm_config.method = NormalizationConfig::Method::Z_SCORE;
    Normalizer normalizer(norm_config);

    normalizer.fit(data);
    auto normalized = normalizer.transform(data);
    ASSERT_TRUE(normalized.is_ok());

    // Then apply PCA
    PCAConfig pca_config;
    pca_config.n_components = 2;
    PCA pca(pca_config);

    auto fit_result = pca.fit(normalized.value());
    EXPECT_TRUE(fit_result.is_ok());

    auto transformed = pca.transform(normalized.value());
    ASSERT_TRUE(transformed.is_ok());

    EXPECT_EQ(transformed.value().cols(), 2);
}

TEST_F(IntegrationTest, StatisticalTestsOnReturns) {
    // Generate return series
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 0.01);

    std::vector<double> returns(100);
    for (size_t i = 0; i < returns.size(); ++i) {
        returns[i] = d(gen);
    }

    // Test stationarity
    ADFTestConfig adf_config;
    ADFTest adf(adf_config);

    auto adf_result = adf.test(returns);
    ASSERT_TRUE(adf_result.is_ok());
    EXPECT_TRUE(adf_result.value().reject_null);  // Returns should be stationary

    // Fit GARCH
    GARCHConfig garch_config;
    GARCH garch(garch_config);

    auto fit_result = garch.fit(returns);
    EXPECT_TRUE(fit_result.is_ok());

    auto vol = garch.get_current_volatility();
    ASSERT_TRUE(vol.is_ok());
    EXPECT_GT(vol.value(), 0.0);
}
