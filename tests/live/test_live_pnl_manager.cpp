// Coverage for live_pnl_manager.cpp. Targets:
// - finalize_previous_day: empty positions, missing T-2 prices, missing T-1
//   for symbol, missing T-2 for symbol, full happy path
// - calculate_position_pnls: missing prices skip + cumulative PnL accounting
// - update_position_pnl with realized
// - get_current_snapshot
// - get_point_value: registry hit, fallback, unknown
// - reset_daily_tracking / set_total_pnl

#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

// Pre-load std headers before flipping private→public.
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/instruments/futures.hpp"
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
#undef private

using namespace trade_ngin;

namespace {

Position make_position(const std::string& symbol, double qty) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(0.0);
    return p;
}

std::shared_ptr<FuturesInstrument> make_es_futures() {
    FuturesSpec spec;
    spec.root_symbol = "ES";
    spec.exchange = "CME";
    spec.currency = "USD";
    spec.multiplier = 50.0;
    spec.tick_size = 0.25;
    spec.commission_per_contract = 2.5;
    spec.initial_margin = 12000.0;
    spec.maintenance_margin = 10000.0;
    spec.weight = 1.0;
    return std::make_shared<FuturesInstrument>("MES", spec);
}

}  // namespace

class LivePnLManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto& reg = InstrumentRegistry::instance();
        reg.instruments_["MES"] = make_es_futures();
    }
    void TearDown() override {
        auto& reg = InstrumentRegistry::instance();
        reg.instruments_.erase("MES");
    }
};

// ===== finalize_previous_day =====

TEST_F(LivePnLManagerTest, FinalizeEmptyPositionsSucceedsWithEmptyResult) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    auto r = mgr.finalize_previous_day({}, {}, {}, 500000.0, 0.0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().success);
    EXPECT_TRUE(r.value().finalized_positions.empty());
}

TEST_F(LivePnLManagerTest, FinalizeMissingAllT2PricesReturnsError) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    std::vector<Position> positions{make_position("ES", 1.0)};
    auto r = mgr.finalize_previous_day(positions, {{"ES", 4500.0}}, {}, 500000.0, 0.0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(LivePnLManagerTest, FinalizeMissingT1ForSymbolRecordsZeroPnL) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    std::vector<Position> positions{make_position("ES", 1.0)};
    // T-2 has price but T-1 missing → continuity fallback
    auto r = mgr.finalize_previous_day(positions, {}, {{"ES", 4490.0}}, 500000.0, 0.0);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().finalized_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("ES"), 0.0);
}

TEST_F(LivePnLManagerTest, FinalizeMissingT2ForSymbolRecordsZeroPnL) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    std::vector<Position> positions{make_position("ES", 1.0)};
    // T-1 has price but T-2 missing → fallback (after the all-empty check)
    auto r = mgr.finalize_previous_day(positions, {{"ES", 4500.0}},
                                       {{"OTHER", 4490.0}},  // populated but missing ES
                                       500000.0, 0.0);
    ASSERT_TRUE(r.is_ok());
    ASSERT_EQ(r.value().finalized_positions.size(), 1u);
    EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("ES"), 0.0);
}

TEST_F(LivePnLManagerTest, FinalizeHappyPathComputesPnLAndPortfolio) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    std::vector<Position> positions{make_position("ES", 2.0)};
    auto r = mgr.finalize_previous_day(positions, {{"ES", 4510.0}}, {{"ES", 4500.0}},
                                       500000.0, 5.0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().success);
    // PnL = 2 * (4510 - 4500) * 50 = 1000
    EXPECT_DOUBLE_EQ(r.value().finalized_daily_pnl, 1000.0);
    // Net = 1000 - 5 = 995, portfolio = 500000 + 995 = 500995
    EXPECT_DOUBLE_EQ(r.value().finalized_portfolio_value, 500995.0);
    EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("ES"), 1000.0);
}

// ===== calculate_position_pnls =====

TEST_F(LivePnLManagerTest, CalculatePositionPnLsSkipsMissingPrices) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    std::vector<Position> positions{
        make_position("ES", 1.0),    // both prices present
        make_position("CL", 1.0),    // missing prices
    };
    auto r = mgr.calculate_position_pnls(positions, {{"ES", 4510.0}}, {{"ES", 4500.0}});
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(mgr.get_position_daily_pnl("ES"), 500.0);  // 1 * 10 * 50
    EXPECT_DOUBLE_EQ(mgr.get_position_daily_pnl("CL"), 0.0);  // skipped
    EXPECT_DOUBLE_EQ(mgr.get_total_daily_pnl(), 500.0);
}

TEST_F(LivePnLManagerTest, CalculatePositionPnLsResetsBeforeAccumulating) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    std::vector<Position> positions{make_position("ES", 1.0)};
    mgr.calculate_position_pnls(positions, {{"ES", 4510.0}}, {{"ES", 4500.0}});
    EXPECT_DOUBLE_EQ(mgr.get_total_daily_pnl(), 500.0);
    // Run again with a different price → should reset, not double up.
    mgr.calculate_position_pnls(positions, {{"ES", 4505.0}}, {{"ES", 4500.0}});
    EXPECT_DOUBLE_EQ(mgr.get_total_daily_pnl(), 250.0);
}

// ===== update_position_pnl =====

TEST_F(LivePnLManagerTest, UpdatePositionPnLAccumulatesIntoCumulative) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    mgr.update_position_pnl("ES", 100.0, 50.0);
    mgr.update_position_pnl("CL", -25.0, 0.0);
    EXPECT_DOUBLE_EQ(mgr.get_total_daily_pnl(), 75.0);
    EXPECT_DOUBLE_EQ(mgr.get_position_realized_pnl("ES"), 50.0);
}

TEST_F(LivePnLManagerTest, UpdatePositionPnLZeroRealizedDoesNotOverwrite) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    mgr.update_position_pnl("ES", 100.0, 200.0);
    EXPECT_DOUBLE_EQ(mgr.get_position_realized_pnl("ES"), 200.0);
    mgr.update_position_pnl("ES", 100.0, 0.0);  // 0 → don't overwrite
    EXPECT_DOUBLE_EQ(mgr.get_position_realized_pnl("ES"), 200.0);
}

// ===== get_current_snapshot =====

TEST_F(LivePnLManagerTest, GetCurrentSnapshotPopulatesFields) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    mgr.set_total_pnl(1234.0);
    mgr.update_position_pnl("ES", 100.0, 50.0);
    auto r = mgr.get_current_snapshot();
    ASSERT_TRUE(r.is_ok());
    auto& s = r.value();
    EXPECT_DOUBLE_EQ(s.daily_pnl, 100.0);
    EXPECT_DOUBLE_EQ(s.total_pnl, 1234.0);
    EXPECT_DOUBLE_EQ(s.realized_pnl, 50.0);
    EXPECT_DOUBLE_EQ(s.unrealized_pnl, 0.0);
    EXPECT_DOUBLE_EQ(s.portfolio_value, 500000.0 + 1234.0);
}

// ===== get_point_value =====

TEST_F(LivePnLManagerTest, GetPointValueFromRegistry) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    EXPECT_DOUBLE_EQ(mgr.get_point_value("ES"), 50.0);  // registry resolves ES → MES → 50.0
}

TEST_F(LivePnLManagerTest, GetPointValueFallbackForKnownSymbolNotInRegistry) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    // CL not registered, falls back to 1000.0
    EXPECT_DOUBLE_EQ(mgr.get_point_value("CL"), 1000.0);
}

TEST_F(LivePnLManagerTest, GetPointValueUnknownSymbolReturnsOne) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    EXPECT_DOUBLE_EQ(mgr.get_point_value("UNKNOWN_XYZ"), 1.0);
}

TEST_F(LivePnLManagerTest, GetPointValueStripsVariantSuffix) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    // ES.v.0 should normalize to ES → MES → 50.0
    EXPECT_DOUBLE_EQ(mgr.get_point_value("ES.v.0"), 50.0);
}

// ===== reset_daily_tracking =====

TEST_F(LivePnLManagerTest, ResetDailyTrackingClearsAccumulators) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    mgr.update_position_pnl("ES", 100.0, 50.0);
    EXPECT_GT(mgr.get_total_daily_pnl(), 0.0);
    mgr.reset_daily_tracking();
    EXPECT_DOUBLE_EQ(mgr.get_total_daily_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(mgr.get_position_daily_pnl("ES"), 0.0);
    EXPECT_DOUBLE_EQ(mgr.get_position_realized_pnl("ES"), 0.0);
}
