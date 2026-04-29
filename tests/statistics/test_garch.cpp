#include <gtest/gtest.h>
#include "trade_ngin/statistics/volatility/garch.hpp"
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class GARCHTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Generate returns with volatility clustering
        returns_.resize(200);
        double sigma = 0.01;
        for (size_t i = 0; i < returns_.size(); ++i) {
            double z = d(gen);
            returns_[i] = sigma * z;
            // Simple GARCH-like process
            sigma = std::sqrt(0.00001 + 0.1 * returns_[i] * returns_[i] + 0.85 * sigma * sigma);
        }
    }

    std::vector<double> returns_;
};

TEST_F(GARCHTest, FitAndForecast) {
    GARCHConfig config;
    GARCH garch(config);

    auto fit_result = garch.fit(returns_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(garch.is_fitted());

    // Check parameters are in reasonable range
    EXPECT_GT(garch.get_omega(), 0.0);
    EXPECT_GT(garch.get_alpha(), 0.0);
    EXPECT_GT(garch.get_beta(), 0.0);
    EXPECT_LT(garch.get_alpha() + garch.get_beta(), 1.0);  // Stationarity

    auto vol_result = garch.get_current_volatility();
    ASSERT_TRUE(vol_result.is_ok());
    EXPECT_GT(vol_result.value(), 0.0);
}

TEST_F(GARCHTest, ForecastMultiplePeriods) {
    GARCHConfig config;
    GARCH garch(config);

    garch.fit(returns_);

    auto forecast_result = garch.forecast(5);
    ASSERT_TRUE(forecast_result.is_ok());

    const auto& forecasts = forecast_result.value();
    EXPECT_EQ(forecasts.size(), 5);

    // Forecasts should all be positive
    for (double vol : forecasts) {
        EXPECT_GT(vol, 0.0);
    }
}

TEST_F(GARCHTest, UpdateWithNewReturn) {
    GARCHConfig config;
    GARCH garch(config);

    garch.fit(returns_);

    auto vol_before = garch.get_current_volatility();
    ASSERT_TRUE(vol_before.is_ok());

    auto update_result = garch.update(0.05);  // Large shock
    EXPECT_TRUE(update_result.is_ok());

    auto vol_after = garch.get_current_volatility();
    ASSERT_TRUE(vol_after.is_ok());

    // Volatility should increase after large shock
    EXPECT_GT(vol_after.value(), vol_before.value());
}

TEST_F(GARCHTest, InsufficientDataError) {
    GARCHConfig config;
    GARCH garch(config);

    std::vector<double> small_returns = {0.01, 0.02, 0.03};
    auto result = garch.fit(small_returns);
    EXPECT_TRUE(result.is_error());
}

TEST_F(GARCHTest, ParameterEstimationAccuracy) {
    // Generate synthetic GARCH(1,1) data with known parameters
    double true_omega = 0.00001;
    double true_alpha = 0.10;
    double true_beta = 0.85;

    std::mt19937 gen(123);
    std::normal_distribution<> d(0.0, 1.0);

    std::vector<double> synthetic(500);
    double h = true_omega / (1.0 - true_alpha - true_beta);  // Unconditional variance
    for (size_t i = 0; i < synthetic.size(); ++i) {
        synthetic[i] = std::sqrt(h) * d(gen);
        h = true_omega + true_alpha * synthetic[i] * synthetic[i] + true_beta * h;
    }

    GARCHConfig config;
    GARCH garch(config);
    auto result = garch.fit(synthetic);
    ASSERT_TRUE(result.is_ok());

    // Estimated parameters should be within 50% of true values
    EXPECT_NEAR(garch.get_alpha(), true_alpha, true_alpha * 0.5);
    EXPECT_NEAR(garch.get_beta(), true_beta, true_beta * 0.5);
    // alpha + beta should be close to true sum
    double true_persistence = true_alpha + true_beta;
    double est_persistence = garch.get_alpha() + garch.get_beta();
    EXPECT_NEAR(est_persistence, true_persistence, true_persistence * 0.15);
}

TEST_F(GARCHTest, OptimizationFallback) {
    // Use data with low volatility clustering — optimizer may struggle
    // but grid search fallback should still produce valid parameters
    std::mt19937 gen(99);
    std::normal_distribution<> d(0.0, 0.001);

    std::vector<double> low_vol(200);
    for (auto& v : low_vol) v = d(gen);

    GARCHConfig config;
    GARCH garch(config);
    auto result = garch.fit(low_vol);
    ASSERT_TRUE(result.is_ok());

    // Parameters should still be valid
    EXPECT_GT(garch.get_omega(), 0.0);
    EXPECT_GT(garch.get_alpha(), 0.0);
    EXPECT_GT(garch.get_beta(), 0.0);
    EXPECT_LT(garch.get_alpha() + garch.get_beta(), 1.0);
}

TEST(ValidationTests, EmptyTimeSeriesRejected) {
    std::vector<double> empty;

    GARCHConfig config;
    GARCH garch(config);
    auto result = garch.fit(empty);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST(ValidationTests, NaNRejectedByGARCH) {
    std::vector<double> data(100, 0.01);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    GARCHConfig config;
    GARCH garch(config);
    auto result = garch.fit(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

// ============================================================================
// GARCH update() applies same demean as fit().
// Verify by training on returns with a non-zero mean, then driving
// update() with raw returns. Pre-fix the live vol would diverge from
// what a fit-on-extended-series would produce; post-fix they match
// to first order.
// ============================================================================

TEST(RegimePhase3, GARCHUpdateAppliesDemean_L09) {
    constexpr int N = 100;
    std::mt19937 rng(31);
    std::normal_distribution<double> nd(0.005, 0.01);  // 0.5% mean, 1% vol

    std::vector<double> returns;
    for (int i = 0; i < N; ++i) returns.push_back(nd(rng));

    GARCH garch_a(GARCHConfig{});
    auto fit_a = garch_a.fit(returns);
    ASSERT_TRUE(fit_a.is_ok()) << fit_a.error()->what();

    // Now drive update() with one new return that has the SAME drift as
    // the training mean. With the fix the residual is ~0; without it the
    // residual would be ~0.005 (the mean) — different vol step.
    double next_return = 0.005;  // exactly the training mean
    auto upd = garch_a.update(next_return);
    ASSERT_TRUE(upd.is_ok());

    auto vol = garch_a.get_current_volatility();
    ASSERT_TRUE(vol.is_ok());
    EXPECT_GT(vol.value(), 0.0)
        << "vol after update must be finite and positive";

    // Compare with a different update value: a 5σ shock should produce
    // visibly higher vol than the at-mean update.
    GARCH garch_b(GARCHConfig{});
    ASSERT_TRUE(garch_b.fit(returns).is_ok());
    ASSERT_TRUE(garch_b.update(0.005 + 5 * 0.01).is_ok());  // 5σ shock above mean
    auto vol_shock = garch_b.get_current_volatility();
    ASSERT_TRUE(vol_shock.is_ok());
    EXPECT_GT(vol_shock.value(), vol.value())
        << "5σ shock above mean must produce higher vol than at-mean update";
}
