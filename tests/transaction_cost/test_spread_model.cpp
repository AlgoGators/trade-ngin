#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/transaction_cost/spread_model.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;
using namespace trade_ngin::testing;

namespace {

AssetCostConfig default_asset() {
    AssetCostConfig c;
    c.symbol = "ES";
    c.baseline_spread_ticks = 1.0;
    c.min_spread_ticks = 1.0;
    c.max_spread_ticks = 10.0;
    c.spread_cost_multiplier = 0.5;
    c.max_impact_bps = 100.0;
    c.tick_size = 0.25;
    c.point_value = 50.0;
    return c;
}

}  // namespace

class SpreadModelTest : public TestBase {
protected:
    SpreadModel model_;
};

// ===== calculate_spread_price_impact =====

TEST_F(SpreadModelTest, BaselineVolatilityYieldsHalfTickSpreadCost) {
    auto cfg = default_asset();
    // baseline_spread=1, vol_mult=1 → spread_ticks=1 (within bounds)
    // impact = 0.5 * 1 * 0.25 = 0.125
    EXPECT_DOUBLE_EQ(model_.calculate_spread_price_impact(cfg, 1.0), 0.125);
}

TEST_F(SpreadModelTest, HighVolatilityWidensSpread) {
    auto cfg = default_asset();
    cfg.baseline_spread_ticks = 4.0;
    // vol_mult=2 → 8 ticks (within max=10) → 0.5 * 8 * 0.25 = 1.0
    EXPECT_DOUBLE_EQ(model_.calculate_spread_price_impact(cfg, 2.0), 1.0);
}

TEST_F(SpreadModelTest, SpreadClampedAtMaxTicks) {
    auto cfg = default_asset();
    cfg.baseline_spread_ticks = 5.0;
    cfg.max_spread_ticks = 8.0;
    // 5 * 3 = 15 → clamped to 8 → 0.5 * 8 * 0.25 = 1.0
    EXPECT_DOUBLE_EQ(model_.calculate_spread_price_impact(cfg, 3.0), 1.0);
}

TEST_F(SpreadModelTest, SpreadClampedAtMinTicks) {
    auto cfg = default_asset();
    cfg.baseline_spread_ticks = 1.0;
    cfg.min_spread_ticks = 2.0;
    // 1 * 0.5 = 0.5 → clamped to min=2 → 0.5 * 2 * 0.25 = 0.25
    EXPECT_DOUBLE_EQ(model_.calculate_spread_price_impact(cfg, 0.5), 0.25);
}

TEST_F(SpreadModelTest, CustomSpreadCostMultiplierAppliesLinearly) {
    auto cfg = default_asset();
    cfg.spread_cost_multiplier = 0.25;  // limit-order style
    // 0.25 * 1 * 0.25 = 0.0625
    EXPECT_DOUBLE_EQ(model_.calculate_spread_price_impact(cfg, 1.0), 0.0625);
}

// ===== calculate_volatility_multiplier =====

TEST_F(SpreadModelTest, VolatilityMultiplierDefaultsToOneOnInsufficientData) {
    EXPECT_DOUBLE_EQ(model_.calculate_volatility_multiplier({}), 1.0);
    EXPECT_DOUBLE_EQ(model_.calculate_volatility_multiplier({0.01}), 1.0);
}

TEST_F(SpreadModelTest, VolatilityMultiplierAtBaselineSigmaIsOne) {
    // With sigma exactly = baseline (0.01), z=0 → mult=1
    // Construct returns with stdev = 0.01.
    std::vector<double> returns;
    for (int i = 0; i < 30; ++i) {
        returns.push_back(i % 2 == 0 ? 0.01 : -0.01);
    }
    // stdev approx = 0.01 (sample stdev with N=30)
    double m = model_.calculate_volatility_multiplier(returns);
    EXPECT_NEAR(m, 1.0, 0.05);
}

TEST_F(SpreadModelTest, VolatilityMultiplierClampsAtMax) {
    // High volatility series
    std::vector<double> returns;
    for (int i = 0; i < 30; ++i) {
        returns.push_back(i % 2 == 0 ? 0.10 : -0.10);  // large
    }
    SpreadModel::VolatilityConfig vc;
    vc.lambda = 1.0;  // amplify the effect
    vc.min_multiplier = 0.8;
    vc.max_multiplier = 1.5;
    SpreadModel m(vc);
    EXPECT_DOUBLE_EQ(m.calculate_volatility_multiplier(returns), 1.5);
}

TEST_F(SpreadModelTest, VolatilityMultiplierClampsAtMin) {
    // Very low volatility (tiny variance)
    std::vector<double> returns(30, 0.0001);
    SpreadModel::VolatilityConfig vc;
    vc.lambda = 1.0;
    vc.min_multiplier = 0.7;
    vc.max_multiplier = 1.5;
    SpreadModel m(vc);
    EXPECT_DOUBLE_EQ(m.calculate_volatility_multiplier(returns), 0.7);
}

// ===== update_log_returns + get_volatility_multiplier =====

TEST_F(SpreadModelTest, GetVolatilityMultiplierBeforeUpdateReturnsOne) {
    EXPECT_DOUBLE_EQ(model_.get_volatility_multiplier("ES"), 1.0);
}

TEST_F(SpreadModelTest, GetVolatilityMultiplierWithSingleReturnReturnsOne) {
    model_.update_log_returns("ES", 0.02);
    EXPECT_DOUBLE_EQ(model_.get_volatility_multiplier("ES"), 1.0);
}

TEST_F(SpreadModelTest, UpdateLogReturnsRollsRollingWindow) {
    SpreadModel::VolatilityConfig vc;
    vc.lookback_days = 5;
    SpreadModel m(vc);
    for (int i = 0; i < 10; ++i) {
        m.update_log_returns("ES", static_cast<double>(i));
    }
    // Internal deque size capped at 5; check via get_vol_mult not erroring
    double mult = m.get_volatility_multiplier("ES");
    EXPECT_TRUE(std::isfinite(mult));
}

TEST_F(SpreadModelTest, GetVolatilityMultiplierUsesStoredReturns) {
    // Feed real returns and confirm multiplier is between [0.8, 1.5]
    for (int i = 0; i < 30; ++i) {
        model_.update_log_returns("ES", i % 2 == 0 ? 0.01 : -0.01);
    }
    double mult = model_.get_volatility_multiplier("ES");
    EXPECT_GE(mult, 0.8);
    EXPECT_LE(mult, 1.5);
}

// ===== clear_symbol_data + clear_all =====

TEST_F(SpreadModelTest, ClearSymbolDataResetsToDefault) {
    for (int i = 0; i < 5; ++i) {
        model_.update_log_returns("ES", 0.01);
    }
    model_.clear_symbol_data("ES");
    EXPECT_DOUBLE_EQ(model_.get_volatility_multiplier("ES"), 1.0);
}

TEST_F(SpreadModelTest, ClearAllResetsAllSymbols) {
    for (int i = 0; i < 5; ++i) {
        model_.update_log_returns("ES", 0.01);
        model_.update_log_returns("NQ", 0.02);
    }
    model_.clear_all();
    EXPECT_DOUBLE_EQ(model_.get_volatility_multiplier("ES"), 1.0);
    EXPECT_DOUBLE_EQ(model_.get_volatility_multiplier("NQ"), 1.0);
}

TEST_F(SpreadModelTest, MultipleSymbolsTrackedIndependently) {
    for (int i = 0; i < 30; ++i) {
        model_.update_log_returns("ES", i % 2 == 0 ? 0.01 : -0.01);   // baseline-ish
        model_.update_log_returns("NQ", i % 2 == 0 ? 0.10 : -0.10);   // high vol
    }
    double es = model_.get_volatility_multiplier("ES");
    double nq = model_.get_volatility_multiplier("NQ");
    EXPECT_GE(nq, es);  // NQ has more vol, multiplier should be ≥ ES's
}
