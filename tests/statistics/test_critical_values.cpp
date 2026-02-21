#include <gtest/gtest.h>
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/tests/adf_test.hpp"
#include "trade_ngin/statistics/tests/kpss_test.hpp"
#include <cmath>
#include <random>
#include <limits>
#include <numeric>

using namespace trade_ngin::statistics;

TEST(ADFCriticalValues, SmallSampleMoreNegativeThanLarge) {
    // For any regression type, small-sample CV should be more negative
    double cv_small = trade_ngin::statistics::critical_values::interpolate_adf_cv(25, 1, 0.05);
    double cv_large = trade_ngin::statistics::critical_values::interpolate_adf_cv(500, 1, 0.05);
    EXPECT_LT(cv_small, cv_large);
}

TEST(ADFCriticalValues, ConstantTrendMoreNegativeThanConstant) {
    double cv_constant = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 1, 0.05);
    double cv_trend = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 2, 0.05);
    EXPECT_LT(cv_trend, cv_constant);
}

TEST(ADFCriticalValues, NoConstantWorks) {
    double cv = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 0, 0.05);
    EXPECT_LT(cv, 0.0);
    // No-constant CVs should be less negative than constant CVs
    double cv_constant = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 1, 0.05);
    EXPECT_GT(cv, cv_constant);
}

TEST(ADFCriticalValues, InterpolationBetweenSampleSizes) {
    // CV at n=75 should be between n=50 and n=100
    double cv_50 = trade_ngin::statistics::critical_values::interpolate_adf_cv(50, 1, 0.05);
    double cv_75 = trade_ngin::statistics::critical_values::interpolate_adf_cv(75, 1, 0.05);
    double cv_100 = trade_ngin::statistics::critical_values::interpolate_adf_cv(100, 1, 0.05);
    // CVs become less negative as n increases, so cv_50 < cv_75 < cv_100
    EXPECT_LT(cv_50, cv_75);
    EXPECT_LT(cv_75, cv_100);
}

TEST(ADFCriticalValues, RegressionTypeAffectsTestResult) {
    // Generate white noise — should be stationary regardless of regression type
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    std::vector<double> data(200);
    for (auto& v : data) v = d(gen);

    ADFTestConfig config_const;
    config_const.regression = ADFTestConfig::RegressionType::CONSTANT;
    ADFTest adf_const(config_const);
    auto r1 = adf_const.test(data);
    ASSERT_TRUE(r1.is_ok());

    ADFTestConfig config_trend;
    config_trend.regression = ADFTestConfig::RegressionType::CONSTANT_TREND;
    ADFTest adf_trend(config_trend);
    auto r2 = adf_trend.test(data);
    ASSERT_TRUE(r2.is_ok());

    // Trend CV should be more negative
    EXPECT_LT(r2.value().critical_value, r1.value().critical_value);
}

TEST(JohansenCriticalValues, ThreeSeriesMatchTable) {
    auto cv = trade_ngin::statistics::critical_values::johansen_trace_critical_values(3, 0.05);
    ASSERT_EQ(cv.size(), 3);
    EXPECT_DOUBLE_EQ(cv[0], 29.68);
    EXPECT_DOUBLE_EQ(cv[1], 15.41);
    EXPECT_DOUBLE_EQ(cv[2], 3.76);
}

TEST(JohansenCriticalValues, OnePercentMoreStringent) {
    auto cv_5 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(2, 0.05);
    auto cv_1 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(2, 0.01);
    // 1% CVs should be larger (harder to reject)
    for (size_t i = 0; i < cv_5.size(); ++i) {
        EXPECT_GT(cv_1[i], cv_5[i]);
    }
}

TEST(JohansenCriticalValues, FourAndFiveSeriesWork) {
    auto cv4 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(4, 0.05);
    ASSERT_EQ(cv4.size(), 4);
    EXPECT_DOUBLE_EQ(cv4[0], 47.21);

    auto cv5 = trade_ngin::statistics::critical_values::johansen_trace_critical_values(5, 0.05);
    ASSERT_EQ(cv5.size(), 5);
    EXPECT_DOUBLE_EQ(cv5[0], 68.52);
}

TEST(LogSumExp, BasicProperties) {
    using trade_ngin::statistics::critical_values::log_sum_exp;

    // log(exp(0) + exp(0)) = log(2)
    EXPECT_NEAR(log_sum_exp(0.0, 0.0), std::log(2.0), 1e-12);

    // log(exp(-1000) + exp(0)) ≈ 0
    EXPECT_NEAR(log_sum_exp(-1000.0, 0.0), 0.0, 1e-12);

    // log(exp(1000) + exp(0)) ≈ 1000
    EXPECT_NEAR(log_sum_exp(1000.0, 0.0), 1000.0, 1e-12);

    // -inf + x = x
    double neg_inf = -std::numeric_limits<double>::infinity();
    EXPECT_DOUBLE_EQ(log_sum_exp(neg_inf, 5.0), 5.0);
}

TEST(LogSumExp, ArrayVersion) {
    using trade_ngin::statistics::critical_values::log_sum_exp;

    double values[] = {-1000.0, 0.0, -1000.0};
    EXPECT_NEAR(log_sum_exp(values, 3), 0.0, 1e-12);

    double values2[] = {1.0, 2.0, 3.0};
    double expected = std::log(std::exp(1.0) + std::exp(2.0) + std::exp(3.0));
    EXPECT_NEAR(log_sum_exp(values2, 3), expected, 1e-12);
}

// ============================================================================
// ADF P-Value Tests
// ============================================================================

TEST(ADFPValue, StationarySeriesSmallPValue) {
    // White noise should be stationary → ADF should reject → small p-value
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    std::vector<double> data(200);
    for (auto& v : data) v = d(gen);

    trade_ngin::statistics::ADFTestConfig config;
    config.regression = trade_ngin::statistics::ADFTestConfig::RegressionType::CONSTANT;
    trade_ngin::statistics::ADFTest adf(config);
    auto result = adf.test(data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_LT(result.value().p_value, 0.05);
    EXPECT_GT(result.value().p_value, 0.0);
}

TEST(ADFPValue, NonStationarySeriesLargePValue) {
    // Random walk should be non-stationary → ADF should not reject → large p-value
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    std::vector<double> data(200);
    data[0] = 0.0;
    for (size_t i = 1; i < data.size(); ++i) {
        data[i] = data[i-1] + d(gen);
    }

    trade_ngin::statistics::ADFTestConfig config;
    config.regression = trade_ngin::statistics::ADFTestConfig::RegressionType::CONSTANT;
    trade_ngin::statistics::ADFTest adf(config);
    auto result = adf.test(data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_GT(result.value().p_value, 0.05);
}

TEST(ADFPValue, MonotonicInStatistic) {
    // More negative statistic → smaller p-value
    using trade_ngin::statistics::critical_values::approximate_adf_p_value;

    double p1 = approximate_adf_p_value(-5.0, 100, 1);  // Very negative
    double p2 = approximate_adf_p_value(-3.0, 100, 1);  // Moderately negative
    double p3 = approximate_adf_p_value(-1.0, 100, 1);  // Barely negative

    EXPECT_LE(p1, p2);
    EXPECT_LE(p2, p3);
}

// ============================================================================
// KPSS P-Value Tests
// ============================================================================

TEST(KPSSPValue, StationarySeriesLargePValue) {
    // White noise should be stationary → KPSS should not reject → large p-value
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    std::vector<double> data(200);
    for (auto& v : data) v = d(gen);

    trade_ngin::statistics::KPSSTestConfig config;
    config.regression = trade_ngin::statistics::KPSSTestConfig::RegressionType::CONSTANT;
    trade_ngin::statistics::KPSSTest kpss(config);
    auto result = kpss.test(data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_GT(result.value().p_value, 0.05);
}

TEST(KPSSPValue, NonStationarySeriesSmallPValue) {
    // Random walk with drift should be non-stationary → KPSS should reject → small p-value
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    std::vector<double> data(500);
    data[0] = 0.0;
    for (size_t i = 1; i < data.size(); ++i) {
        data[i] = data[i-1] + d(gen);
    }

    trade_ngin::statistics::KPSSTestConfig config;
    config.regression = trade_ngin::statistics::KPSSTestConfig::RegressionType::CONSTANT;
    config.max_lags = 4;  // Use shorter lags to avoid inflating long-run variance estimate
    trade_ngin::statistics::KPSSTest kpss(config);
    auto result = kpss.test(data);
    ASSERT_TRUE(result.is_ok());
    EXPECT_LT(result.value().p_value, 0.05);
}
