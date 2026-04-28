#include <gtest/gtest.h>
#include <cmath>
#include "../core/test_base.hpp"
#include "trade_ngin/transaction_cost/impact_model.hpp"

using namespace trade_ngin;
using namespace trade_ngin::transaction_cost;
using namespace trade_ngin::testing;

namespace {

AssetCostConfig default_asset() {
    AssetCostConfig c;
    c.symbol = "ES";
    c.tick_size = 0.25;
    c.point_value = 50.0;
    c.max_impact_bps = 100.0;
    return c;
}

}  // namespace

class ImpactModelTest : public TestBase {
protected:
    ImpactModel model_;
};

// ===== get_impact_k_bps: liquidity bucket dispatch =====

TEST_F(ImpactModelTest, GetImpactKBpsUltraLiquidBucket) {
    EXPECT_DOUBLE_EQ(model_.get_impact_k_bps(2'000'000.0), 10.0);
}

TEST_F(ImpactModelTest, GetImpactKBpsLiquidBucket) {
    EXPECT_DOUBLE_EQ(model_.get_impact_k_bps(500'000.0), 20.0);
}

TEST_F(ImpactModelTest, GetImpactKBpsMediumBucket) {
    EXPECT_DOUBLE_EQ(model_.get_impact_k_bps(100'000.0), 40.0);
}

TEST_F(ImpactModelTest, GetImpactKBpsThinBucket) {
    EXPECT_DOUBLE_EQ(model_.get_impact_k_bps(30'000.0), 60.0);
}

TEST_F(ImpactModelTest, GetImpactKBpsVeryThinBucket) {
    EXPECT_DOUBLE_EQ(model_.get_impact_k_bps(10'000.0), 80.0);
    EXPECT_DOUBLE_EQ(model_.get_impact_k_bps(0.0), 80.0);
}

// ===== calculate_market_impact =====

TEST_F(ImpactModelTest, CalculateImpactUsesAbsoluteQuantity) {
    auto cfg = default_asset();
    double pos = model_.calculate_market_impact(100.0, 4000.0, 1'000'000.0, cfg);
    double neg = model_.calculate_market_impact(-100.0, 4000.0, 1'000'000.0, cfg);
    EXPECT_DOUBLE_EQ(pos, neg);
    EXPECT_GT(pos, 0.0);
}

TEST_F(ImpactModelTest, CalculateImpactClampedAtMaxParticipation) {
    auto cfg = default_asset();
    // qty = 200,000, ADV = 100,000 → participation 2.0 → clamped to 0.1
    // ultra liquid path? ADV=100k → MEDIUM bucket → k=40
    // impact = 40 * sqrt(0.1) = 12.65 bps; capped at 100
    double impact = model_.calculate_market_impact(200'000.0, 100.0, 100'000.0, cfg);
    double expected = (40.0 * std::sqrt(0.1) / 10000.0) * 100.0;
    EXPECT_NEAR(impact, expected, 1e-9);
}

TEST_F(ImpactModelTest, CalculateImpactCappedByMaxImpactBps) {
    auto cfg = default_asset();
    cfg.max_impact_bps = 1.0;  // very low cap
    // Compute would be larger than 1 bps but capped to 1
    double impact = model_.calculate_market_impact(50'000.0, 100.0, 100'000.0, cfg);
    double expected = (1.0 / 10000.0) * 100.0;
    EXPECT_DOUBLE_EQ(impact, expected);
}

TEST_F(ImpactModelTest, CalculateImpactWithZeroAdvUsesMinAdv) {
    auto cfg = default_asset();
    // ADV=0 floor lifts to min_adv=100; qty=10 → 10/100 = 0.1 (max_partic)
    // ADV=100 → very thin bucket k=80; impact = 80 * sqrt(0.1) = 25.3 bps; capped at 100
    double impact = model_.calculate_market_impact(10.0, 100.0, 0.0, cfg);
    double expected = (80.0 * std::sqrt(0.1) / 10000.0) * 100.0;
    EXPECT_NEAR(impact, expected, 1e-9);
}

TEST_F(ImpactModelTest, CalculateImpactScalesWithReferencePrice) {
    auto cfg = default_asset();
    double low = model_.calculate_market_impact(100.0, 50.0, 1'000'000.0, cfg);
    double high = model_.calculate_market_impact(100.0, 500.0, 1'000'000.0, cfg);
    EXPECT_NEAR(high / low, 10.0, 1e-9);
}

TEST_F(ImpactModelTest, CalculateImpactScalesWithSqrtParticipation) {
    auto cfg = default_asset();
    // Same bucket, same price, qty 4× → impact 2× (sqrt of 4)
    double small = model_.calculate_market_impact(100.0, 4000.0, 1'000'000.0, cfg);
    double large = model_.calculate_market_impact(400.0, 4000.0, 1'000'000.0, cfg);
    EXPECT_NEAR(large / small, 2.0, 1e-9);
}

// ===== update_volume + get_adv =====

TEST_F(ImpactModelTest, GetAdvBeforeAnyUpdateReturnsZero) {
    EXPECT_DOUBLE_EQ(model_.get_adv("ES"), 0.0);
}

TEST_F(ImpactModelTest, GetAdvAveragesPushedVolumes) {
    model_.update_volume("ES", 100.0);
    model_.update_volume("ES", 200.0);
    model_.update_volume("ES", 300.0);
    EXPECT_DOUBLE_EQ(model_.get_adv("ES"), 200.0);
}

TEST_F(ImpactModelTest, UpdateVolumeRollsRollingWindow) {
    ImpactModel::Config c;
    c.adv_lookback_days = 3;
    ImpactModel m(c);
    m.update_volume("ES", 100.0);
    m.update_volume("ES", 200.0);
    m.update_volume("ES", 300.0);
    m.update_volume("ES", 400.0);  // pushes 100 out
    // ADV = (200+300+400)/3 = 300
    EXPECT_DOUBLE_EQ(m.get_adv("ES"), 300.0);
}

TEST_F(ImpactModelTest, MultipleSymbolsTrackedIndependently) {
    model_.update_volume("ES", 1'000'000.0);
    model_.update_volume("NQ", 500'000.0);
    EXPECT_DOUBLE_EQ(model_.get_adv("ES"), 1'000'000.0);
    EXPECT_DOUBLE_EQ(model_.get_adv("NQ"), 500'000.0);
}

// ===== has_sufficient_data =====

TEST_F(ImpactModelTest, HasSufficientDataFalseForUnknownSymbol) {
    EXPECT_FALSE(model_.has_sufficient_data("UNKNOWN", 5));
}

TEST_F(ImpactModelTest, HasSufficientDataReflectsDayCount) {
    model_.update_volume("ES", 100.0);
    model_.update_volume("ES", 200.0);
    EXPECT_TRUE(model_.has_sufficient_data("ES", 1));
    EXPECT_TRUE(model_.has_sufficient_data("ES", 2));
    EXPECT_FALSE(model_.has_sufficient_data("ES", 3));
}

// ===== clear_* =====

TEST_F(ImpactModelTest, ClearSymbolDataResetsAdv) {
    model_.update_volume("ES", 1000.0);
    EXPECT_GT(model_.get_adv("ES"), 0.0);
    model_.clear_symbol_data("ES");
    EXPECT_DOUBLE_EQ(model_.get_adv("ES"), 0.0);
}

TEST_F(ImpactModelTest, ClearAllResetsAllSymbols) {
    model_.update_volume("ES", 1000.0);
    model_.update_volume("NQ", 500.0);
    model_.clear_all();
    EXPECT_DOUBLE_EQ(model_.get_adv("ES"), 0.0);
    EXPECT_DOUBLE_EQ(model_.get_adv("NQ"), 0.0);
}
