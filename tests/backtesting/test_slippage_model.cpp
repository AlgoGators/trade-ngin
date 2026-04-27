#include <gtest/gtest.h>
#include <cmath>
#include <memory>
#include <optional>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/slippage_models.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Bar make_bar(const std::string& symbol, double price, double volume,
             double high_factor = 1.01, double low_factor = 0.99) {
    Bar bar;
    bar.timestamp = Timestamp{};
    bar.symbol = symbol;
    bar.open = Decimal(price);
    bar.high = Decimal(price * high_factor);
    bar.low = Decimal(price * low_factor);
    bar.close = Decimal(price);
    bar.volume = volume;
    return bar;
}

VolumeSlippageConfig default_volume_config() {
    VolumeSlippageConfig c;
    c.price_impact_coefficient = 1e-3;
    c.min_volume_ratio = 0.01;
    c.max_volume_ratio = 0.1;
    c.volatility_multiplier = 1.5;
    return c;
}

SpreadSlippageConfig default_spread_config() {
    SpreadSlippageConfig c;
    c.min_spread_bps = 1.0;
    c.spread_multiplier = 1.2;
    c.market_impact_multiplier = 1.5;
    return c;
}

}  // namespace

class VolumeSlippageModelTest : public TestBase {
protected:
    VolumeSlippageConfig config_ = default_volume_config();
};

TEST_F(VolumeSlippageModelTest, NoMarketDataBuyAddsLinearImpact) {
    VolumeSlippageModel m(config_);
    // impact = |qty| * coef = 1000 * 1e-3 = 1.0 → BUY: price * 2.0
    double slipped = m.calculate_slippage(100.0, 1000.0, Side::BUY, std::nullopt);
    EXPECT_DOUBLE_EQ(slipped, 200.0);
}

TEST_F(VolumeSlippageModelTest, NoMarketDataSellSubtractsLinearImpact) {
    VolumeSlippageModel m(config_);
    // impact = 1.0 → SELL: price * 0.0
    double slipped = m.calculate_slippage(100.0, 1000.0, Side::SELL, std::nullopt);
    EXPECT_DOUBLE_EQ(slipped, 0.0);
}

TEST_F(VolumeSlippageModelTest, NoMarketDataNegativeQtyUsesAbsoluteValue) {
    VolumeSlippageModel m(config_);
    double pos = m.calculate_slippage(100.0, 500.0, Side::BUY, std::nullopt);
    double neg = m.calculate_slippage(100.0, -500.0, Side::BUY, std::nullopt);
    EXPECT_DOUBLE_EQ(pos, neg);
}

TEST_F(VolumeSlippageModelTest, MarketDataWithoutStoredAvgFallsBackToBarVolume) {
    VolumeSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 10000.0);
    // volume_ratio raw = 100/10000 = 0.01, clamped to [0.01, 0.1] => 0.01
    // base_impact = 1e-3 * sqrt(0.01) * 1.0 = 1e-4
    double slipped = m.calculate_slippage(100.0, 100.0, Side::BUY, bar);
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 1e-4), 1e-9);
}

TEST_F(VolumeSlippageModelTest, MarketDataUsesStoredAvgAfterUpdate) {
    VolumeSlippageModel m(config_);
    Bar update_bar = make_bar("ES", 100.0, 50000.0);
    m.update(update_bar);  // first update sets avg_volume = 50000
    Bar query_bar = make_bar("ES", 100.0, 10000.0);
    // volume_ratio = 500 / 50000 = 0.01 (clamped to min)
    // base_impact = 1e-3 * sqrt(0.01) * vol_adjust
    // First update set volatility = (1.01-0.99)/1.0 = 0.02; vol_adjust = 0.02 * 1.5 = 0.03
    double slipped = m.calculate_slippage(100.0, 500.0, Side::BUY, query_bar);
    double expected_impact = 1e-3 * std::sqrt(0.01) * 0.02 * 1.5;
    EXPECT_NEAR(slipped, 100.0 * (1.0 + expected_impact), 1e-9);
}

TEST_F(VolumeSlippageModelTest, MinVolumeRatioClampApplied) {
    VolumeSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 1'000'000.0);
    // raw volume_ratio = 1/1e6 = 1e-6, well below min=0.01
    // After clamp: 0.01; base_impact = 1e-3 * sqrt(0.01) * 1.0 = 1e-4
    double slipped = m.calculate_slippage(100.0, 1.0, Side::BUY, bar);
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 1e-4), 1e-9);
}

TEST_F(VolumeSlippageModelTest, MaxVolumeRatioClampApplied) {
    VolumeSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 1000.0);
    // raw volume_ratio = 5000/1000 = 5.0; clamps to max=0.1
    // base_impact = 1e-3 * sqrt(0.1) * 1.0 ≈ 3.1623e-4
    double slipped = m.calculate_slippage(100.0, 5000.0, Side::BUY, bar);
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 1e-3 * std::sqrt(0.1)), 1e-9);
}

TEST_F(VolumeSlippageModelTest, MarketDataSellSubtractsImpact) {
    VolumeSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 10000.0);
    // raw ratio = 100/10000 = 0.01 (== min, no clamp change), base_impact = 1e-3 * sqrt(0.01) = 1e-4
    double slipped = m.calculate_slippage(100.0, 100.0, Side::SELL, bar);
    EXPECT_NEAR(slipped, 100.0 * (1.0 - 1e-4), 1e-9);
}

TEST_F(VolumeSlippageModelTest, ExcessVolumeBranchIsUnreachableAfterClamp) {
    // FIXME: production bug — VolumeSlippageModel::calculate_slippage clamps
    // volume_ratio to [min_volume_ratio, max_volume_ratio] on line 30 and then
    // checks `if (volume_ratio > max_volume_ratio)` on line 41. After the clamp
    // the condition can never be true, so the "excess_ratio" extra-impact branch
    // is dead code. Capture observed behavior: a heavily oversized order produces
    // exactly the max-clamp impact, with no excess multiplier applied.
    VolumeSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 1000.0);
    double clamped = m.calculate_slippage(100.0, 5'000'000.0, Side::BUY, bar);
    double expected = 100.0 * (1.0 + 1e-3 * std::sqrt(0.1));
    EXPECT_NEAR(clamped, expected, 1e-9)
        << "If this fires, the dead-branch FIXME above may be resolved.";
}

TEST_F(VolumeSlippageModelTest, UpdateFirstCallStoresExactBarVolume) {
    VolumeSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 12345.0);
    m.update(bar);
    // After first update, calling on a bar with bar.volume=1 (which would
    // otherwise drive a huge ratio) should now use the stored 12345 average.
    Bar tiny = make_bar("ES", 100.0, 1.0);
    // raw volume_ratio = 100/12345 ≈ 8.1e-3, below min, clamps to 0.01
    double slipped = m.calculate_slippage(100.0, 100.0, Side::BUY, tiny);
    double expected_impact = 1e-3 * std::sqrt(0.01) * 0.02 * 1.5;
    EXPECT_NEAR(slipped, 100.0 * (1.0 + expected_impact), 1e-9);
}

TEST_F(VolumeSlippageModelTest, UpdateSubsequentAppliesRollingAverage) {
    VolumeSlippageModel m(config_);
    m.update(make_bar("ES", 100.0, 1000.0));   // avg_volume = 1000
    m.update(make_bar("ES", 100.0, 21000.0));  // avg = (1000*19 + 21000)/20 = 2000
    // Verify by computing slippage using a known formula and known stored avg.
    // raw ratio = 200/2000 = 0.1 (== max), so clamps to 0.1.
    // base_impact = 1e-3 * sqrt(0.1) * vol_adjust
    // After two updates, volatility EWMA: vol1 = 0.02, vol2 = 0.9*0.02 + 0.1*0.02 = 0.02
    // vol_adjust = 0.02 * 1.5 = 0.03
    double slipped =
        m.calculate_slippage(100.0, 200.0, Side::BUY, make_bar("ES", 100.0, 5000.0));
    double expected_impact = 1e-3 * std::sqrt(0.1) * 0.03;
    EXPECT_NEAR(slipped, 100.0 * (1.0 + expected_impact), 1e-9);
}

TEST_F(VolumeSlippageModelTest, UpdateKeepsSymbolsIndependent) {
    VolumeSlippageModel m(config_);
    m.update(make_bar("ES", 100.0, 50000.0));
    m.update(make_bar("NQ", 100.0, 1000.0));
    // ES query: stored avg = 50000, ratio = 500/50000 = 0.01 (min)
    double es = m.calculate_slippage(100.0, 500.0, Side::BUY, make_bar("ES", 100.0, 99.0));
    // NQ query: stored avg = 1000, ratio = 500/1000 = 0.5, clamps to 0.1
    double nq = m.calculate_slippage(100.0, 500.0, Side::BUY, make_bar("NQ", 100.0, 99.0));
    EXPECT_GT(nq, es);  // larger participation → larger impact
}

TEST_F(VolumeSlippageModelTest, UpdateVolatilityEWMARespondsToBarRange) {
    VolumeSlippageModel m(config_);
    // First update: high/low spread of 0.02 → volatility = 0.02
    m.update(make_bar("ES", 100.0, 1000.0, 1.01, 0.99));
    // Second update with much wider range; EWMA = 0.9*0.02 + 0.1*0.20 = 0.038
    m.update(make_bar("ES", 100.0, 1000.0, 1.10, 0.90));
    Bar query = make_bar("ES", 100.0, 1000.0);
    // ratio = 100/1000 = 0.1 (max)
    double slipped = m.calculate_slippage(100.0, 100.0, Side::BUY, query);
    double expected_impact = 1e-3 * std::sqrt(0.1) * 0.038 * 1.5;
    EXPECT_NEAR(slipped, 100.0 * (1.0 + expected_impact), 1e-9);
}

TEST_F(VolumeSlippageModelTest, DecimalOverloadDelegatesToDoubleImpl) {
    VolumeSlippageModel m(config_);
    // Decimal overload lives on the base class and is name-hidden by the
    // derived double override; reach it via a base reference.
    SlippageModel& base = m;
    Decimal d = base.calculate_slippage(Decimal(100.0), Decimal(1000.0), Side::BUY, std::nullopt);
    double dbl = m.calculate_slippage(100.0, 1000.0, Side::BUY, std::nullopt);
    EXPECT_DOUBLE_EQ(static_cast<double>(d), dbl);
}

class SpreadSlippageModelTest : public TestBase {
protected:
    SpreadSlippageConfig config_ = default_spread_config();
};

TEST_F(SpreadSlippageModelTest, NoMarketDataBuyUsesMinSpread) {
    SpreadSlippageModel m(config_);
    // adjusted_spread = 1.0 * 1.2 = 1.2 bps → impact = 1.2/10000 = 1.2e-4
    double slipped = m.calculate_slippage(100.0, 1000.0, Side::BUY, std::nullopt);
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 1.2e-4), 1e-9);
}

TEST_F(SpreadSlippageModelTest, NoMarketDataSellSubtractsImpact) {
    SpreadSlippageModel m(config_);
    double slipped = m.calculate_slippage(100.0, 1000.0, Side::SELL, std::nullopt);
    EXPECT_NEAR(slipped, 100.0 * (1.0 - 1.2e-4), 1e-9);
}

TEST_F(SpreadSlippageModelTest, MarketDataNoStoredEstimateUsesMinSpread) {
    SpreadSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 10000.0);
    // Without prior update, no stored estimate, so spread_bps = min_spread_bps.
    // volume_ratio = 500/10000 = 0.05, below 0.1 threshold → no impact bump.
    double slipped = m.calculate_slippage(100.0, 500.0, Side::BUY, bar);
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 1.2e-4), 1e-9);
}

TEST_F(SpreadSlippageModelTest, MarketDataStoredEstimateBelowMinKeepsMin) {
    // Force a tiny stored estimate via update() then verify max(min, stored) = min.
    SpreadSlippageConfig cfg = config_;
    cfg.min_spread_bps = 50.0;  // raise floor above the synthetic 0.02% estimate
    SpreadSlippageModel m(cfg);
    m.update(make_bar("ES", 100.0, 10000.0, 1.0001, 0.9999));  // est ≈ 2 bps
    double slipped =
        m.calculate_slippage(100.0, 500.0, Side::BUY, make_bar("ES", 100.0, 10000.0));
    // adjusted = max(50, 2) * 1.2 = 60; impact = 60/10000 = 6e-3
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 6e-3), 1e-9);
}

TEST_F(SpreadSlippageModelTest, MarketDataStoredEstimateAboveMinUsesStored) {
    SpreadSlippageModel m(config_);
    // First update sets spread_estimate = (high-low)/close * 10000 = 200 bps
    m.update(make_bar("ES", 100.0, 10000.0, 1.01, 0.99));
    Bar query = make_bar("ES", 100.0, 10000.0);
    // ratio = 500/10000 = 0.05 (below threshold) so no impact bump
    // adjusted_spread = max(1, 200) * 1.2 = 240 bps; impact = 240/10000 = 0.024
    double slipped = m.calculate_slippage(100.0, 500.0, Side::BUY, query);
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 0.024), 1e-9);
}

TEST_F(SpreadSlippageModelTest, MarketDataVolumeImpactOnlyAboveTenPercent) {
    SpreadSlippageModel m(config_);
    Bar bar = make_bar("ES", 100.0, 10000.0);
    // qty=1000 → ratio=0.1 (== threshold, NOT >, so no extra)
    double at_thresh = m.calculate_slippage(100.0, 1000.0, Side::BUY, bar);
    // qty=2000 → ratio=0.2 → extra factor (1 + 1.5*(0.2-0.1)) = 1.15
    double over = m.calculate_slippage(100.0, 2000.0, Side::BUY, bar);
    EXPECT_NEAR(at_thresh, 100.0 * (1.0 + 1.2e-4), 1e-9);
    EXPECT_NEAR(over, 100.0 * (1.0 + 1.2e-4 * 1.15), 1e-9);
    EXPECT_GT(over, at_thresh);
}

TEST_F(SpreadSlippageModelTest, UpdateFirstSetsExactEstimate) {
    SpreadSlippageModel m(config_);
    m.update(make_bar("ES", 100.0, 10000.0, 1.005, 0.995));
    // estimate = (1.005-0.995)/1.0 * 10000 = 100 bps; impact = 100*1.2/10000 = 0.012
    double slipped =
        m.calculate_slippage(100.0, 500.0, Side::BUY, make_bar("ES", 100.0, 10000.0));
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 0.012), 1e-9);
}

TEST_F(SpreadSlippageModelTest, UpdateSubsequentSmoothsEstimate) {
    SpreadSlippageModel m(config_);
    m.update(make_bar("ES", 100.0, 10000.0, 1.005, 0.995));  // est = 100 bps
    m.update(make_bar("ES", 100.0, 10000.0, 1.020, 0.980));  // est_new = 400; smoothed = 0.95*100 + 0.05*400 = 115
    double slipped =
        m.calculate_slippage(100.0, 500.0, Side::BUY, make_bar("ES", 100.0, 10000.0));
    // adjusted = 115 * 1.2 = 138; impact = 0.0138
    EXPECT_NEAR(slipped, 100.0 * (1.0 + 0.0138), 1e-9);
}

TEST_F(SpreadSlippageModelTest, DecimalOverloadDelegatesToDoubleImpl) {
    SpreadSlippageModel m(config_);
    SlippageModel& base = m;
    Decimal d = base.calculate_slippage(Decimal(100.0), Decimal(500.0), Side::BUY, std::nullopt);
    double dbl = m.calculate_slippage(100.0, 500.0, Side::BUY, std::nullopt);
    EXPECT_DOUBLE_EQ(static_cast<double>(d), dbl);
}

TEST(SlippageModelFactoryTest, CreatesVolumeModelOfCorrectType) {
    auto model = SlippageModelFactory::create_volume_model(default_volume_config());
    ASSERT_NE(model, nullptr);
    EXPECT_NE(dynamic_cast<VolumeSlippageModel*>(model.get()), nullptr);
    EXPECT_EQ(dynamic_cast<SpreadSlippageModel*>(model.get()), nullptr);
}

TEST(SlippageModelFactoryTest, CreatesSpreadModelOfCorrectType) {
    auto model = SlippageModelFactory::create_spread_model(default_spread_config());
    ASSERT_NE(model, nullptr);
    EXPECT_NE(dynamic_cast<SpreadSlippageModel*>(model.get()), nullptr);
    EXPECT_EQ(dynamic_cast<VolumeSlippageModel*>(model.get()), nullptr);
}
