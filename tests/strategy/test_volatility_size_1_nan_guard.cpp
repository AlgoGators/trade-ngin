#include <gtest/gtest.h>
#include <cmath>
#include <deque>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Regression test for audit finding §1.6: calculate_volatility() previously
// allowed prices.size() == 2 to slip past the early-out guard, producing one
// return and then dividing by (returns.size() - 1) == 0, yielding NaN that
// propagated through vol-scaled position sizing.
class VolatilitySize1NaNGuardTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        strategy_config_.trading_params["AAPL"] = 1.0;
        strategy_config_.position_limits["AAPL"] = 1000.0;

        mr_config_.lookback_period = 20;
        mr_config_.vol_lookback = 20;
        mr_config_.entry_threshold = 2.0;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;

        strategy_ = std::make_unique<MeanReversionStrategy>(
            "TEST_VOL_GUARD", strategy_config_, mr_config_, db_);
        ASSERT_TRUE(strategy_->initialize().is_ok());
    }

    void TearDown() override {
        if (strategy_) {
            strategy_->stop();
            strategy_.reset();
        }
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    MeanReversionConfig mr_config_;
    std::unique_ptr<MeanReversionStrategy> strategy_;
};

TEST_F(VolatilitySize1NaNGuardTest, EmptyPriceDequeReturnsDefault) {
    std::deque<double> prices;
    double v = strategy_->calculate_volatility_for_test(prices, 20);
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_DOUBLE_EQ(v, 0.01);
}

TEST_F(VolatilitySize1NaNGuardTest, SinglePriceReturnsDefault) {
    std::deque<double> prices{100.0};
    double v = strategy_->calculate_volatility_for_test(prices, 20);
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_DOUBLE_EQ(v, 0.01);
}

// The audit's primary scenario: 2 prices → 1 return → division by 0 → NaN
// pre-fix. Post-fix this must return the default 0.01.
TEST_F(VolatilitySize1NaNGuardTest, TwoPricesReturnsDefaultNotNaN) {
    std::deque<double> prices{100.0, 101.0};
    double v = strategy_->calculate_volatility_for_test(prices, 20);
    EXPECT_TRUE(std::isfinite(v)) << "vol must not be NaN with 2 prices";
    EXPECT_DOUBLE_EQ(v, 0.01);
}

// Sparse data where prices[i-1] <= 0 skips entries, leaving only one valid return.
TEST_F(VolatilitySize1NaNGuardTest, SparseValidPricesLeavingOneReturnYieldsDefault) {
    std::deque<double> prices{0.0, 0.0, 100.0, 101.0};
    double v = strategy_->calculate_volatility_for_test(prices, 20);
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_DOUBLE_EQ(v, 0.01);
}

// Sanity: with enough data the function computes a non-default value.
TEST_F(VolatilitySize1NaNGuardTest, EnoughPricesComputesRealValue) {
    std::deque<double> prices;
    for (int i = 0; i < 30; ++i) {
        prices.push_back(100.0 + i * 0.5);
    }
    double v = strategy_->calculate_volatility_for_test(prices, 20);
    EXPECT_TRUE(std::isfinite(v));
    EXPECT_GT(v, 0.0);
}
