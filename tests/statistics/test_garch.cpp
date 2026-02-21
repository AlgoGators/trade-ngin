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
