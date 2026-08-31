// Coverage for margin_manager.cpp. Targets:
// - Static math helpers (gross_leverage, equity_to_margin_ratio, margin_cushion)
// - normalize_symbol_for_lookup variant-suffix stripping
// - calculate_margin_requirements happy path with a single futures instrument
//   injected directly into the InstrumentRegistry singleton via #define
//   private public
// - validate_margins ok / warning / error returns
// - calculate_position_notional / calculate_position_margin error path on
//   unknown symbol

#include <gtest/gtest.h>

// Pre-load std headers (including ranges and the others used inside the
// trade_ngin headers) BEFORE flipping private→public. Otherwise libc++
// internals stop compiling.
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
#include "trade_ngin/live/margin_manager.hpp"
#undef private

using namespace trade_ngin;

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg_price);
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
    return std::make_shared<FuturesInstrument>("ES", spec);
}

}  // namespace

class MarginManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Hand-inject futures instrument. NOTE: get_instrument("ES") looks up
        // "MES" internally (the micro contract), so we register under MES.
        auto& reg = InstrumentRegistry::instance();
        reg.instruments_["MES"] = make_es_futures();
    }
    void TearDown() override {
        auto& reg = InstrumentRegistry::instance();
        reg.instruments_.erase("MES");
    }
};

// ===== Static math helpers =====

TEST_F(MarginManagerTest, GrossLeverageIsZeroForNonPositivePortfolio) {
    EXPECT_DOUBLE_EQ(MarginManager::calculate_gross_leverage(100000.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(MarginManager::calculate_gross_leverage(100000.0, -1.0), 0.0);
}

TEST_F(MarginManagerTest, GrossLeverageIsRatio) {
    EXPECT_DOUBLE_EQ(MarginManager::calculate_gross_leverage(200000.0, 100000.0), 2.0);
}

TEST_F(MarginManagerTest, EquityToMarginIsZeroForNonPositiveMargin) {
    EXPECT_DOUBLE_EQ(MarginManager::calculate_equity_to_margin_ratio(100000.0, 0.0), 0.0);
}

TEST_F(MarginManagerTest, EquityToMarginIsRatio) {
    EXPECT_DOUBLE_EQ(
        MarginManager::calculate_equity_to_margin_ratio(120000.0, 30000.0), 4.0);
}

TEST_F(MarginManagerTest, MarginCushionIsRatioMinusOneTimes100) {
    EXPECT_DOUBLE_EQ(MarginManager::calculate_margin_cushion(2.5), 150.0);
    EXPECT_DOUBLE_EQ(MarginManager::calculate_margin_cushion(1.0), 0.0);
    EXPECT_DOUBLE_EQ(MarginManager::calculate_margin_cushion(0.5), -50.0);
}

// ===== normalize_symbol_for_lookup =====

TEST_F(MarginManagerTest, NormalizeStripsVariantSuffix) {
    EXPECT_EQ(MarginManager::normalize_symbol_for_lookup("ES.v.0"), "ES");
}

TEST_F(MarginManagerTest, NormalizeStripsContinuousSuffix) {
    EXPECT_EQ(MarginManager::normalize_symbol_for_lookup("ES.c.0"), "ES");
}

TEST_F(MarginManagerTest, NormalizeLeavesPlainSymbolUnchanged) {
    EXPECT_EQ(MarginManager::normalize_symbol_for_lookup("ES"), "ES");
}

// ===== calculate_margin_requirements =====

TEST_F(MarginManagerTest, EmptyPositionsProducesZeroMetrics) {
    MarginManager mm(InstrumentRegistry::instance());
    auto r = mm.calculate_margin_requirements({}, {}, 100000.0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().active_positions, 0);
    EXPECT_DOUBLE_EQ(r.value().gross_notional, 0.0);
    EXPECT_DOUBLE_EQ(r.value().total_posted_margin, 0.0);
    // cash_available = portfolio - posted = 100000
    EXPECT_DOUBLE_EQ(r.value().cash_available, 100000.0);
}

TEST_F(MarginManagerTest, ZeroQuantityPositionIsSkipped) {
    MarginManager mm(InstrumentRegistry::instance());
    std::unordered_map<std::string, Position> positions{
        {"ES", make_position("ES", 0.0, 4500.0)}};
    auto r = mm.calculate_margin_requirements(positions, {}, 100000.0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().active_positions, 0);
}

TEST_F(MarginManagerTest, SinglePositionPopulatesAllFields) {
    MarginManager mm(InstrumentRegistry::instance());
    std::unordered_map<std::string, Position> positions{
        {"ES", make_position("ES", 2.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = mm.calculate_margin_requirements(positions, prices, 1'000'000.0);
    ASSERT_TRUE(r.is_ok());
    auto& m = r.value();
    EXPECT_EQ(m.active_positions, 1);
    EXPECT_DOUBLE_EQ(m.total_posted_margin, 2.0 * 12000.0);  // qty * initial margin
    EXPECT_DOUBLE_EQ(m.maintenance_requirement, 2.0 * 10000.0);
    // notional = qty * price * multiplier = 2 * 4500 * 50 = 450000
    EXPECT_DOUBLE_EQ(m.gross_notional, 450'000.0);
    EXPECT_DOUBLE_EQ(m.net_notional, 450'000.0);
    // gross_leverage = 450000 / 1M = 0.45
    EXPECT_NEAR(m.gross_leverage, 0.45, 1e-9);
    EXPECT_GT(m.equity_to_margin_ratio, 0.0);
    EXPECT_DOUBLE_EQ(m.cash_available, 1'000'000.0 - 24000.0);
}

TEST_F(MarginManagerTest, ShortPositionContributesToGrossNotionalAbsolute) {
    MarginManager mm(InstrumentRegistry::instance());
    std::unordered_map<std::string, Position> positions{
        {"ES", make_position("ES", -3.0, 4500.0)}};
    std::unordered_map<std::string, double> prices{{"ES", 4500.0}};
    auto r = mm.calculate_margin_requirements(positions, prices, 1'000'000.0);
    ASSERT_TRUE(r.is_ok());
    auto& m = r.value();
    EXPECT_DOUBLE_EQ(m.gross_notional, 3.0 * 4500.0 * 50.0);
    EXPECT_LT(m.net_notional, 0.0);
}

TEST_F(MarginManagerTest, MissingInstrumentReturnsError) {
    MarginManager mm(InstrumentRegistry::instance());
    std::unordered_map<std::string, Position> positions{
        {"UNKNOWN_SYM", make_position("UNKNOWN_SYM", 1.0, 100.0)}};
    auto r = mm.calculate_margin_requirements(positions, {}, 100000.0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(MarginManagerTest, MissingMarketPriceFallsBackToAveragePrice) {
    MarginManager mm(InstrumentRegistry::instance());
    std::unordered_map<std::string, Position> positions{
        {"ES", make_position("ES", 1.0, 4400.0)}};
    auto r = mm.calculate_margin_requirements(positions, {}, 1'000'000.0);
    ASSERT_TRUE(r.is_ok());
    // notional = 1 * 4400 * 50 = 220000 (fell back to average price)
    EXPECT_DOUBLE_EQ(r.value().gross_notional, 220'000.0);
}

// ===== validate_margins =====

TEST_F(MarginManagerTest, ValidateMarginsErrorsOnZeroMarginWithActivePositions) {
    MarginManager mm(InstrumentRegistry::instance());
    MarginManager::MarginMetrics m;
    m.active_positions = 1;
    m.total_posted_margin = 0.0;
    EXPECT_TRUE(mm.validate_margins(m).is_error());
}

TEST_F(MarginManagerTest, ValidateMarginsOkOnHealthyMetrics) {
    MarginManager mm(InstrumentRegistry::instance());
    MarginManager::MarginMetrics m;
    m.active_positions = 1;
    m.total_posted_margin = 10000.0;
    m.equity_to_margin_ratio = 5.0;
    EXPECT_TRUE(mm.validate_margins(m).is_ok());
}

TEST_F(MarginManagerTest, ValidateMarginsOkOnEmptyPortfolio) {
    MarginManager mm(InstrumentRegistry::instance());
    MarginManager::MarginMetrics m;  // active_positions=0, no error
    EXPECT_TRUE(mm.validate_margins(m).is_ok());
}

// ===== calculate_position_notional / margin error paths =====

TEST_F(MarginManagerTest, CalculatePositionNotionalUnknownSymbolErrors) {
    MarginManager mm(InstrumentRegistry::instance());
    auto r = mm.calculate_position_notional("UNKNOWN_SYM", 1.0, 100.0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(MarginManagerTest, CalculatePositionMarginUnknownSymbolErrors) {
    MarginManager mm(InstrumentRegistry::instance());
    auto r = mm.calculate_position_margin("UNKNOWN_SYM", 1.0, 100.0);
    EXPECT_TRUE(r.is_error());
}
