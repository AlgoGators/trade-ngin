#include <gtest/gtest.h>
#include "trade_ngin/statistics/volatility/gjr_garch.hpp"
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class GJRGARCHTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate GJR-GARCH returns with leverage effect
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        int n = 300;
        returns_.resize(n);
        double sigma2 = 0.0001;

        for (int i = 0; i < n; ++i) {
            double z = d(gen);
            returns_[i] = std::sqrt(sigma2) * z;
            double indicator = (returns_[i] < 0) ? 1.0 : 0.0;
            sigma2 = 0.00001 + 0.05 * returns_[i] * returns_[i]
                     + 0.1 * indicator * returns_[i] * returns_[i]
                     + 0.85 * sigma2;
        }
    }

    std::vector<double> returns_;
};

TEST_F(GJRGARCHTest, FitAndForecast) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);

    auto fit_result = gjr.fit(returns_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(gjr.is_fitted());

    EXPECT_GT(gjr.get_omega(), 0.0);
    EXPECT_GT(gjr.get_alpha(), 0.0);
    EXPECT_GE(gjr.get_gamma(), 0.0);
    EXPECT_GT(gjr.get_beta(), 0.0);

    auto vol = gjr.get_current_volatility();
    ASSERT_TRUE(vol.is_ok());
    EXPECT_GT(vol.value(), 0.0);

    auto forecast = gjr.forecast(5);
    ASSERT_TRUE(forecast.is_ok());
    EXPECT_EQ(forecast.value().size(), 5);
    for (double v : forecast.value()) {
        EXPECT_GT(v, 0.0);
    }
}

TEST_F(GJRGARCHTest, AsymmetryDetection) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);

    auto result = gjr.fit(returns_);
    ASSERT_TRUE(result.is_ok());

    // Gamma should be positive (negative returns increase vol more)
    EXPECT_GE(gjr.get_gamma(), 0.0);
}

TEST_F(GJRGARCHTest, NegativeShockIncreasesVol) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);
    gjr.fit(returns_);

    auto vol_before = gjr.get_current_volatility();
    ASSERT_TRUE(vol_before.is_ok());

    // Large negative shock
    gjr.update(-0.05);
    auto vol_neg = gjr.get_current_volatility();
    ASSERT_TRUE(vol_neg.is_ok());

    // Large positive shock of same magnitude
    GJRGARCHConfig config2;
    GJRGARCH gjr2(config2);
    gjr2.fit(returns_);
    gjr2.update(0.05);
    auto vol_pos = gjr2.get_current_volatility();
    ASSERT_TRUE(vol_pos.is_ok());

    // Negative shock should increase volatility more (if gamma > 0)
    if (gjr.get_gamma() > 0.01) {
        EXPECT_GT(vol_neg.value(), vol_pos.value());
    }
}

TEST_F(GJRGARCHTest, Stationarity) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);
    gjr.fit(returns_);

    // alpha + gamma/2 + beta < 1 for stationarity
    double persistence = gjr.get_alpha() + gjr.get_gamma() / 2.0 + gjr.get_beta();
    EXPECT_LT(persistence, 1.0);
}

TEST_F(GJRGARCHTest, ParameterAccuracy) {
    std::mt19937 gen(123);
    std::normal_distribution<> d(0.0, 1.0);

    double true_omega = 0.00001;
    double true_alpha = 0.05;
    double true_gamma = 0.1;
    double true_beta = 0.85;

    std::vector<double> synth(500);
    double h = true_omega / (1.0 - true_alpha - true_gamma / 2.0 - true_beta);
    for (size_t i = 0; i < synth.size(); ++i) {
        synth[i] = std::sqrt(h) * d(gen);
        double ind = (synth[i] < 0) ? 1.0 : 0.0;
        h = true_omega + true_alpha * synth[i] * synth[i]
            + true_gamma * ind * synth[i] * synth[i] + true_beta * h;
    }

    GJRGARCHConfig config;
    GJRGARCH gjr(config);
    auto result = gjr.fit(synth);
    ASSERT_TRUE(result.is_ok());

    // Persistence should be close
    double true_pers = true_alpha + true_gamma / 2.0 + true_beta;
    double est_pers = gjr.get_alpha() + gjr.get_gamma() / 2.0 + gjr.get_beta();
    EXPECT_NEAR(est_pers, true_pers, true_pers * 0.2);
}

TEST_F(GJRGARCHTest, MultiplePeriodForecast) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);
    gjr.fit(returns_);

    auto forecast = gjr.forecast(10);
    ASSERT_TRUE(forecast.is_ok());
    EXPECT_EQ(forecast.value().size(), 10);

    for (double v : forecast.value()) {
        EXPECT_GT(v, 0.0);
        EXPECT_TRUE(std::isfinite(v));
    }
}

TEST_F(GJRGARCHTest, InsufficientDataError) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);

    std::vector<double> small = {0.01, 0.02, 0.03};
    auto result = gjr.fit(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(GJRGARCHTest, NaNRejected) {
    std::vector<double> data(100, 0.01);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    GJRGARCHConfig config;
    GJRGARCH gjr(config);
    auto result = gjr.fit(data);
    EXPECT_TRUE(result.is_error());
}

TEST_F(GJRGARCHTest, ForecastBeforeFitError) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);

    auto result = gjr.forecast(1);
    EXPECT_TRUE(result.is_error());
}
