#include <gtest/gtest.h>
#include "trade_ngin/strategy/forecast_scaler.hpp"
#include "../core/test_base.hpp"
#include <memory>
#include <vector>
#include <cmath>

using namespace trade_ngin;

class ForecastScalerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        config_.volatility_lookback = 252;      // Standard 1-year lookback
        config_.ewma_decay = 10.0;             // EWMA decay factor
        config_.base_scalar_trend = 30.0;       // Base trend scalar
        config_.base_scalar_carry = 23.0;       // Base carry scalar
        config_.forecast_cap = 20.0;            // Standard forecast cap

        scaler_ = std::make_unique<ForecastScaler>(config_);
    }

    void TearDown() override {
        scaler_.reset();
        TestBase::TearDown();
    }

    // Helper to generate synthetic volatility series
    std::vector<double> generate_volatility_series(
        int length,
        double base_vol,
        double trend = 0.0,
        double noise = 0.0) {
        
        std::vector<double> series;
        double vol = base_vol;
        
        for (int i = 0; i < length; ++i) {
            vol += trend;
            if (noise > 0.0) {
                // Add some random noise
                vol += ((i % 2 == 0) ? 1 : -1) * noise;
            }
            series.push_back(std::max(0.01, vol));  // Keep volatility positive
        }
        return series;
    }

    ForecastScalerConfig config_;
    std::unique_ptr<ForecastScaler> scaler_;
};

TEST_F(ForecastScalerTest, BasicScaling) {
    const std::string symbol = "AAPL";
    
    // Update with stable volatility
    scaler_->update_volatility(symbol, 0.2);  // 20% volatility

    // Test trend forecast scaling
    auto trend_result = scaler_->scale_forecast(symbol, 0.5, ForecastType::TREND);
    ASSERT_TRUE(trend_result.is_ok());
    double trend_forecast = trend_result.value();
    
    // Expected scaling: 0.5 * base_scalar_trend * volatility_adjustment
    EXPECT_GT(trend_forecast, 0.0);
    EXPECT_LT(trend_forecast, config_.forecast_cap);

    // Test carry forecast scaling
    auto carry_result = scaler_->scale_forecast(symbol, 0.5, ForecastType::CARRY);
    ASSERT_TRUE(carry_result.is_ok());
    double carry_forecast = carry_result.value();
    
    // Carry forecasts should be scaled differently
    EXPECT_NE(trend_forecast, carry_forecast);
    EXPECT_LT(carry_forecast, trend_forecast)  // Due to lower base scalar
        << "Carry forecasts should be scaled less than trend forecasts";
}

TEST_F(ForecastScalerTest, VolatilityRegimes) {
    const std::string symbol = "MSFT";
    
    // Start with low volatility
    for (int i = 0; i < 10; ++i) {
        scaler_->update_volatility(symbol, 0.15);  // 15% volatility
    }
    
    auto low_vol_result = scaler_->scale_forecast(symbol, 1.0, ForecastType::TREND);
    ASSERT_TRUE(low_vol_result.is_ok());
    double low_vol_forecast = low_vol_result.value();

    // Switch to high volatility
    for (int i = 0; i < 10; ++i) {
        scaler_->update_volatility(symbol, 0.45);  // 45% volatility
    }
    
    auto high_vol_result = scaler_->scale_forecast(symbol, 1.0, ForecastType::TREND);
    ASSERT_TRUE(high_vol_result.is_ok());
    double high_vol_forecast = high_vol_result.value();

    EXPECT_GT(low_vol_forecast, high_vol_forecast)
        << "Forecasts should be scaled down in high volatility regimes";
}

TEST_F(ForecastScalerTest, ForecastCapping) {
    const std::string symbol = "GOOG";
    
    // Use moderate volatility
    scaler_->update_volatility(symbol, 0.25);

    // Test very large raw forecast
    auto large_result = scaler_->scale_forecast(symbol, 5.0, ForecastType::TREND);
    ASSERT_TRUE(large_result.is_ok());
    EXPECT_LE(large_result.value(), config_.forecast_cap)
        << "Large forecasts should be capped";

    // Test very negative forecast
    auto negative_result = scaler_->scale_forecast(symbol, -5.0, ForecastType::TREND);
    ASSERT_TRUE(negative_result.is_ok());
    EXPECT_GE(negative_result.value(), -config_.forecast_cap)
        << "Negative forecasts should be capped";
}

TEST_F(ForecastScalerTest, VolatilitySmoothing) {
    const std::string symbol = "TSLA";
    
    // Generate oscillating volatility series
    auto vols = generate_volatility_series(20, 0.2, 0.0, 0.05);
    
    std::vector<double> forecasts;
    double raw_forecast = 1.0;

    // Process volatilities and collect forecasts
    for (double vol : vols) {
        scaler_->update_volatility(symbol, vol);
        auto result = scaler_->scale_forecast(symbol, raw_forecast, ForecastType::TREND);
        ASSERT_TRUE(result.is_ok());
        forecasts.push_back(result.value());
    }

    // Calculate volatility of forecasts
    double forecast_variance = 0.0;
    double forecast_mean = std::accumulate(forecasts.begin(), forecasts.end(), 0.0) / forecasts.size();
    
    for (double f : forecasts) {
        forecast_variance += (f - forecast_mean) * (f - forecast_mean);
    }
    forecast_variance /= (forecasts.size() - 1);
    double forecast_volatility = std::sqrt(forecast_variance);

    // Input volatility of volatilities
    double vol_variance = 0.0;
    double vol_mean = std::accumulate(vols.begin(), vols.end(), 0.0) / vols.size();
    
    for (double v : vols) {
        vol_variance += (v - vol_mean) * (v - vol_mean);
    }
    vol_variance /= (vols.size() - 1);
    double vol_volatility = std::sqrt(vol_variance);

    EXPECT_LT(forecast_volatility, vol_volatility)
        << "EWMA smoothing should reduce forecast volatility";
}

TEST_F(ForecastScalerTest, TrendRegimeAdaptation) {
    const std::string symbol = "FB";
    
    // Generate trending volatility series
    auto increasing_vols = generate_volatility_series(20, 0.15, 0.01);  // Trending up
    auto decreasing_vols = generate_volatility_series(20, 0.35, -0.01); // Trending down

    // Process increasing volatility trend
    std::vector<double> up_trend_forecasts;
    for (double vol : increasing_vols) {
        scaler_->update_volatility(symbol, vol);
        auto result = scaler_->scale_forecast(symbol, 1.0, ForecastType::TREND);
        ASSERT_TRUE(result.is_ok());
        up_trend_forecasts.push_back(result.value());
    }

    // Verify adaptation to increasing volatility
    for (size_t i = 1; i < up_trend_forecasts.size(); ++i) {
        EXPECT_LE(up_trend_forecasts[i], up_trend_forecasts[i-1])
            << "Forecasts should decrease as volatility increases";
    }

    // Process decreasing volatility trend
    std::vector<double> down_trend_forecasts;
    for (double vol : decreasing_vols) {
        scaler_->update_volatility(symbol, vol);
        auto result = scaler_->scale_forecast(symbol, 1.0, ForecastType::TREND);
        ASSERT_TRUE(result.is_ok());
        down_trend_forecasts.push_back(result.value());
    }

    // Verify adaptation to decreasing volatility
    for (size_t i = 1; i < down_trend_forecasts.size(); ++i) {
        EXPECT_GE(down_trend_forecasts[i], down_trend_forecasts[i-1])
            << "Forecasts should increase as volatility decreases";
    }
}

TEST_F(ForecastScalerTest, QuantileCalculation) {
    const std::string symbol = "NFLX";
    
    // Generate series with known distribution
    std::vector<double> vols = {0.1, 0.15, 0.2, 0.25, 0.3};  // Uniform distribution
    
    // Update volatilities
    for (double vol : vols) {
        scaler_->update_volatility(symbol, vol);
    }

    // Check quantile
    auto quantile_result = scaler_->get_quantile(symbol);
    ASSERT_TRUE(quantile_result.is_ok());
    double quantile = quantile_result.value();

    // For a new volatility at the median, should get quantile near 0.5
    scaler_->update_volatility(symbol, 0.2);
    auto median_quantile_result = scaler_->get_quantile(symbol);
    ASSERT_TRUE(median_quantile_result.is_ok());
    EXPECT_NEAR(median_quantile_result.value(), 0.5, 0.1)
        << "Median value should have quantile near 0.5";

    // For high volatility, should get high quantile
    scaler_->update_volatility(symbol, 0.35);
    auto high_quantile_result = scaler_->get_quantile(symbol);
    ASSERT_TRUE(high_quantile_result.is_ok());
    EXPECT_GT(high_quantile_result.value(), 0.8)
        << "High volatility should have high quantile";
}

TEST_F(ForecastScalerTest, MultipleSymbols) {
    // Test scaling for multiple symbols simultaneously
    std::vector<std::string> symbols = {"SYM1", "SYM2", "SYM3"};
    
    // Update with different volatility regimes
    for (const auto& symbol : symbols) {
        double vol = (symbol == "SYM1") ? 0.15 :   // Low vol
                    (symbol == "SYM2") ? 0.25 :   // Medium vol
                                       0.35;      // High vol
        for (int i = 0; i < 10; ++i) {
            scaler_->update_volatility(symbol, vol);
        }
    }

    // Get scaled forecasts for all symbols
    std::vector<double> forecasts;
    double raw_forecast = 1.0;
    
    for (const auto& symbol : symbols) {
        auto result = scaler_->scale_forecast(symbol, raw_forecast, ForecastType::TREND);
        ASSERT_TRUE(result.is_ok());
        forecasts.push_back(result.value());
    }

    // Verify inverse relationship with volatility
    EXPECT_GT(forecasts[0], forecasts[1]) 
        << "Lower volatility should lead to higher scaling";
    EXPECT_GT(forecasts[1], forecasts[2])
        << "Higher volatility should lead to lower scaling";
}

TEST_F(ForecastScalerTest, VolatilityHistory) {
    const std::string symbol = "AMZN";
    
    // Generate long volatility history
    auto vols = generate_volatility_series(300, 0.2, 0.0, 0.02);
    
    // Update all volatilities
    for (double vol : vols) {
        scaler_->update_volatility(symbol, vol);
    }

    // Get multiplier
    auto mult_result = scaler_->get_multiplier(symbol);
    ASSERT_TRUE(mult_result.is_ok());
    EXPECT_GT(mult_result.value(), 0.0) << "Multiplier should be positive";
    
    // Verify multiplier is using recent history more heavily
    // Add a sequence of high volatility points
    for (int i = 0; i < 5; ++i) {
        scaler_->update_volatility(symbol, 0.4);  // High volatility
    }
    
    auto new_mult_result = scaler_->get_multiplier(symbol);
    ASSERT_TRUE(new_mult_result.is_ok());
    EXPECT_LT(new_mult_result.value(), mult_result.value())
        << "Recent high volatility should reduce multiplier";
}

TEST_F(ForecastScalerTest, EdgeCases) {
    const std::string symbol = "EDGE";

    // Test zero forecast
    auto zero_result = scaler_->scale_forecast(symbol, 0.0, ForecastType::TREND);
    ASSERT_TRUE(zero_result.is_ok());
    EXPECT_DOUBLE_EQ(zero_result.value(), 0.0)
        << "Zero forecast should remain zero after scaling";

    // Test very small volatility
    scaler_->update_volatility(symbol, 0.001);
    auto small_vol_result = scaler_->scale_forecast(symbol, 1.0, ForecastType::TREND);
    ASSERT_TRUE(small_vol_result.is_ok());
    EXPECT_LE(small_vol_result.value(), config_.forecast_cap)
        << "Very small volatility should not cause excessive scaling";

    // Test very large volatility
    scaler_->update_volatility(symbol, 10.0);
    auto large_vol_result = scaler_->scale_forecast(symbol, 1.0, ForecastType::TREND);
    ASSERT_TRUE(large_vol_result.is_ok());
    EXPECT_GT(large_vol_result.value(), 0.0)
        << "Very large volatility should not cause zero scaling";
}

TEST_F(ForecastScalerTest, ConsistencyCheck) {
    const std::string symbol = "CONSIST";
    const double raw_forecast = 1.0;
    
    // Update with consistent volatility
    for (int i = 0; i < 20; ++i) {
        scaler_->update_volatility(symbol, 0.2);
    }

    // Get multiple forecasts
    std::vector<double> forecasts;
    for (int i = 0; i < 10; ++i) {
        auto result = scaler_->scale_forecast(symbol, raw_forecast, ForecastType::TREND);
        ASSERT_TRUE(result.is_ok());
        forecasts.push_back(result.value());
    }

    // Verify all forecasts are the same
    for (size_t i = 1; i < forecasts.size(); ++i) {
        EXPECT_DOUBLE_EQ(forecasts[i], forecasts[0])
            << "Consistent volatility should produce consistent scaling";
    }
}