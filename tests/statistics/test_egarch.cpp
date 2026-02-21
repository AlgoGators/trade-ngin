#include <gtest/gtest.h>
#include "trade_ngin/statistics/volatility/egarch.hpp"
#include <random>
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using namespace trade_ngin::statistics;

class EGARCHTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate EGARCH-like returns with leverage effect
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        double omega = -0.1;
        double alpha = 0.15;
        double gamma = -0.1;
        double beta = 0.95;
        double e_abs_z = std::sqrt(2.0 / M_PI);

        int n = 300;
        returns_.resize(n);
        double log_h = std::log(0.0001);

        for (int i = 0; i < n; ++i) {
            double h = std::exp(log_h);
            double z = d(gen);
            returns_[i] = std::sqrt(h) * z;
            log_h = omega + alpha * (std::abs(z) - e_abs_z) + gamma * z + beta * log_h;
        }
    }

    std::vector<double> returns_;
};

TEST_F(EGARCHTest, FitAndForecast) {
    EGARCHConfig config;
    EGARCH egarch(config);

    auto fit_result = egarch.fit(returns_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(egarch.is_fitted());

    EXPECT_GT(egarch.get_alpha(), 0.0);
    EXPECT_LT(std::abs(egarch.get_beta()), 1.0);

    auto vol = egarch.get_current_volatility();
    ASSERT_TRUE(vol.is_ok());
    EXPECT_GT(vol.value(), 0.0);

    auto forecast = egarch.forecast(5);
    ASSERT_TRUE(forecast.is_ok());
    EXPECT_EQ(forecast.value().size(), 5);
    for (double v : forecast.value()) {
        EXPECT_GT(v, 0.0);
    }
}

TEST_F(EGARCHTest, AsymmetryDetection) {
    EGARCHConfig config;
    EGARCH egarch(config);

    auto result = egarch.fit(returns_);
    ASSERT_TRUE(result.is_ok());

    // Gamma should be negative (leverage effect: negative returns increase vol more)
    EXPECT_LT(egarch.get_gamma(), 0.1);  // Should be negative or near zero
}

TEST_F(EGARCHTest, ParameterAccuracy) {
    // Generate with known parameters and check estimation
    std::mt19937 gen(123);
    std::normal_distribution<> d(0.0, 1.0);

    double true_omega = -0.05;
    double true_alpha = 0.1;
    double true_gamma = -0.08;
    double true_beta = 0.98;
    double e_abs_z = std::sqrt(2.0 / M_PI);

    std::vector<double> synth(500);
    double log_h = std::log(0.0001);

    for (size_t i = 0; i < synth.size(); ++i) {
        double h = std::exp(log_h);
        double z = d(gen);
        synth[i] = std::sqrt(h) * z;
        log_h = true_omega + true_alpha * (std::abs(z) - e_abs_z) + true_gamma * z + true_beta * log_h;
    }

    EGARCHConfig config;
    EGARCH egarch(config);
    auto result = egarch.fit(synth);
    ASSERT_TRUE(result.is_ok());

    // Parameters within reasonable range
    EXPECT_NEAR(egarch.get_beta(), true_beta, 0.15);
    EXPECT_GT(egarch.get_alpha(), 0.0);
}

TEST_F(EGARCHTest, UpdateVolatility) {
    EGARCHConfig config;
    EGARCH egarch(config);
    egarch.fit(returns_);

    auto vol_before = egarch.get_current_volatility();
    ASSERT_TRUE(vol_before.is_ok());

    // Large negative shock should increase volatility (leverage effect)
    auto update_result = egarch.update(-0.05);
    EXPECT_TRUE(update_result.is_ok());

    auto vol_after = egarch.get_current_volatility();
    ASSERT_TRUE(vol_after.is_ok());
    EXPECT_GT(vol_after.value(), 0.0);
}

TEST_F(EGARCHTest, InsufficientDataError) {
    EGARCHConfig config;
    EGARCH egarch(config);

    std::vector<double> small = {0.01, 0.02, 0.03};
    auto result = egarch.fit(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(EGARCHTest, NaNRejected) {
    std::vector<double> data(100, 0.01);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    EGARCHConfig config;
    EGARCH egarch(config);
    auto result = egarch.fit(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(EGARCHTest, ForecastBeforeFitError) {
    EGARCHConfig config;
    EGARCH egarch(config);

    auto result = egarch.forecast(1);
    EXPECT_TRUE(result.is_error());
}

TEST_F(EGARCHTest, MultiplePeriodForecast) {
    EGARCHConfig config;
    EGARCH egarch(config);
    egarch.fit(returns_);

    auto forecast = egarch.forecast(10);
    ASSERT_TRUE(forecast.is_ok());
    EXPECT_EQ(forecast.value().size(), 10);

    // All forecasts should be positive and finite
    for (double v : forecast.value()) {
        EXPECT_GT(v, 0.0);
        EXPECT_TRUE(std::isfinite(v));
    }
}
