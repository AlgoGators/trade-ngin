// T-7b-2 C9a (T-VOL C2): the position buffer's Carver term reads the SIZING weight.
//
// calculate_position sizes an instrument with its own weight (InstrumentData::weight, the
// sector-equal figure get_weights gives it). apply_position_buffer's Carver term,
// 0.1 x capital x idm x risk_target x weight / (contract_size x price x fx x vol), read the flat
// config weight (trend_config_.weight, 0.03 in every shipped portfolio) instead, so the band was
// 10 percent of a position the instrument never holds: 1.74 times too wide for a currency, 1.94
// times for an ag, 0.42 times for MBT. Carver's band is 10 percent of the instrument's OWN average
// position, so the term must use the weight the position used.
//
// Every case here: capital 500,000, idm 2.4, risk_target 0.2, fx 1, contract_size 1, price 100,
// vol 0.2, so the Carver term is 0.1 x 500000 x 2.4 x 0.2 x w / (1 x 100 x 1 x 0.2) = 1200 w
// contracts. The floor and the position factor are 0, so the width IS the Carver term. The
// strategy holds nothing (current 0) and the raw target is 100, so the buffer trades up to the
// lower bound: new position = round(100 - 1200 w). The config weight is 0.03 throughout, which
// would give 1200 x 0.03 = 36 and 64 contracts for every symbol.
//
//   symbol   sizing weight  width = 1200 w   100 - width   stored   (flat 0.03: 64)
//   MBT      0.071429       85.7148          14.2852       14
//   MES      0.038690       46.4280          53.5720       54
//   ZC       0.015476       18.5712          81.4288       81
//   (none)   no instrument data: no sizing weight, Carver term 0, width 0 -> round(100) = 100
//
// The same holds for the Fast and Slow classes, which carry the same buffer code.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

// apply_position_buffer and instrument_data_ are private; reach them directly (the pattern of
// test_portfolio_manager_internals.cpp).
#define private public
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

constexpr double kCapital = 500000.0;
constexpr double kIdm = 2.4;
constexpr double kRiskTarget = 0.2;
constexpr double kFlatConfigWeight = 0.03;
constexpr double kPrice = 100.0;
constexpr double kVol = 0.2;
constexpr double kRaw = 100.0;

StrategyConfig strategy_config() {
    StrategyConfig sc;
    sc.capital_allocation = kCapital;
    sc.max_leverage = 2.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    return sc;
}

template <typename Config>
Config buffer_config() {
    Config tc;
    tc.weight = kFlatConfigWeight;  // the flat config weight the term used to read
    tc.idm = kIdm;
    tc.risk_target = kRiskTarget;
    tc.fx_rate = 1.0;
    tc.use_position_buffering = true;
    tc.carver_buffer_floor = 0.0;             // the width is the Carver term alone
    tc.carver_buffer_position_factor = 0.0;
    return tc;
}

// Seed one instrument's cached sizing data the way on_data does (contract size and weight).
template <typename Strategy>
void seed_instrument(Strategy& s, const std::string& symbol, double sizing_weight) {
    auto& data = s.instrument_data_[symbol];
    data.contract_size = 1.0;
    data.weight = sizing_weight;
}

template <typename Strategy>
void expect_width_follows_the_sizing_weight(Strategy& s) {
    seed_instrument(s, "MBT", 0.071429);
    seed_instrument(s, "MES", 0.038690);
    seed_instrument(s, "ZC", 0.015476);

    EXPECT_EQ(s.apply_position_buffer("MBT", kRaw, kPrice, kVol), 14.0)
        << "width 1200 x 0.071429 = 85.7148 (the flat 0.03 gives 36 and 64)";
    EXPECT_EQ(s.apply_position_buffer("MES", kRaw, kPrice, kVol), 54.0)
        << "width 1200 x 0.038690 = 46.428 (the flat 0.03 gives 36 and 64)";
    EXPECT_EQ(s.apply_position_buffer("ZC", kRaw, kPrice, kVol), 81.0)
        << "width 1200 x 0.015476 = 18.5712 (the flat 0.03 gives 36 and 64)";
    // No instrument data: no sizing weight (calculate_position sizes such a symbol at 0), so the
    // Carver term is 0 and, with the floor and the position term at 0, the width is 0.
    EXPECT_EQ(s.apply_position_buffer("NODATA", kRaw, kPrice, kVol), 100.0)
        << "a symbol with no instrument data has no sizing weight: Carver term 0";
}

class BufferWeightSourceTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        db_ = std::make_shared<MockPostgresDatabase>("mock://buffer_weight");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        db_.reset();
        StateManager::reset_instance();
        TestBase::TearDown();
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

}  // namespace

TEST_F(BufferWeightSourceTest, TrendFollowingCarverTermReadsTheSizingWeight) {
    TrendFollowingStrategy s("BUFFER_WEIGHT_TF", strategy_config(),
                             buffer_config<TrendFollowingConfig>(), db_);
    expect_width_follows_the_sizing_weight(s);
}

TEST_F(BufferWeightSourceTest, TrendFollowingFastCarverTermReadsTheSizingWeight) {
    TrendFollowingFastStrategy s("BUFFER_WEIGHT_FAST", strategy_config(),
                                 buffer_config<TrendFollowingFastConfig>(), db_);
    expect_width_follows_the_sizing_weight(s);
}

TEST_F(BufferWeightSourceTest, TrendFollowingSlowCarverTermReadsTheSizingWeight) {
    TrendFollowingSlowStrategy s("BUFFER_WEIGHT_SLOW", strategy_config(),
                                 buffer_config<TrendFollowingSlowConfig>(), db_);
    expect_width_follows_the_sizing_weight(s);
}

// The width equals the Carver term when floor and factor are 0, and the term is exactly
// 0.1 x capital x idm x risk_target x SIZING weight / (cs x price x fx x vol): a held position
// just inside the band is kept, one just outside is traded to the band's edge.
TEST_F(BufferWeightSourceTest, TheBandEdgeSitsAtTheSizingWeightsWidth) {
    TrendFollowingStrategy s("BUFFER_WEIGHT_EDGE", strategy_config(),
                             buffer_config<TrendFollowingConfig>(), db_);
    seed_instrument(s, "MES", 0.038690);  // width 46.428
    Position held;
    held.symbol = "MES";
    held.quantity = Decimal(54.0);  // 100 - 54 = 46 < 46.428: inside the band, kept
    ASSERT_TRUE(s.update_position("MES", held).is_ok());
    EXPECT_EQ(s.apply_position_buffer("MES", kRaw, kPrice, kVol), 54.0);
    held.quantity = Decimal(53.0);  // 100 - 53 = 47 > 46.428: outside, trade to round(53.572)
    ASSERT_TRUE(s.update_position("MES", held).is_ok());
    EXPECT_EQ(s.apply_position_buffer("MES", kRaw, kPrice, kVol), 54.0);
    held.quantity = Decimal(60.0);  // with the flat 0.03 (width 36) 60 would be outside; kept
    ASSERT_TRUE(s.update_position("MES", held).is_ok());
    EXPECT_EQ(s.apply_position_buffer("MES", kRaw, kPrice, kVol), 60.0);
}
