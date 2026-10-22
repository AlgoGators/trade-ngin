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
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/logger.hpp"
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

// ===== STAT_QUALITY (T-8D F10, T-8D-2 R82): the finalize's own log line =====

namespace {

// The STAT_QUALITY lines a finalize writes to the log, without the logger's prefix.
std::vector<std::string> stat_quality_lines_of(const std::function<void()>& finalize) {
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    lc.include_timestamp = false;
    Logger::instance().initialize(lc);
    ::testing::internal::CaptureStdout();
    finalize();
    const std::string log = ::testing::internal::GetCapturedStdout();
    Logger::instance().set_level(LogLevel::ERR);
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = log.find("STAT_QUALITY", at)) != std::string::npos) {
        const size_t end = log.find('\n', at);
        out.push_back(log.substr(at, end == std::string::npos ? end : end - at));
        at = end == std::string::npos ? log.size() : end;
    }
    return out;
}

}  // namespace

// Five rows: MES, ZL and ZS are held, ZW is held short, GC is a flat carried row. MES and ZW have
// a T-1 close; ZL, ZS and GC have none. Held 4, of them no bar 2 (ZL, ZS); the flat GC is no held
// symbol and is not named.
TEST_F(LivePnLManagerTest, FinalizeLogsOneStatQualityLineNamingTheHeldSymbolsWithNoT1Close) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    const std::vector<Position> positions{make_position("ZS", 1.0), make_position("MES", 2.0),
                                          make_position("GC", 0.0), make_position("ZW", -3.0),
                                          make_position("ZL", 1.0)};
    const std::unordered_map<std::string, double> t1{{"MES", 4510.0}, {"ZW", 600.0}};
    const std::unordered_map<std::string, double> t2{
        {"MES", 4500.0}, {"ZW", 601.0}, {"ZL", 40.0}, {"ZS", 990.0}, {"GC", 2000.0}};
    const auto lines = stat_quality_lines_of([&] {
        auto r = mgr.finalize_previous_day(positions, t1, t2, 500000.0, 0.0);
        ASSERT_TRUE(r.is_ok());
        EXPECT_EQ(r.value().finalized_positions.size(), 5u);
        EXPECT_DOUBLE_EQ(r.value().position_realized_pnl.at("ZL"), 0.0);
    });
    ASSERT_EQ(lines.size(), 1u) << "one line a finalize";
    EXPECT_EQ(lines[0], "STAT_QUALITY finalize held=4 no_bar=2: ZL, ZS");
}

// Every held symbol has its T-1 close: the line is still written, with no_bar=0 and no list. A
// symbol whose bar books nothing but did print (zero_pnl_symbols: a change bar, a withheld bar)
// is not a no-bar symbol.
TEST_F(LivePnLManagerTest, FinalizeLogsTheLineWithZeroWhenEveryHeldSymbolHasItsClose) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    const std::vector<Position> positions{make_position("MES", 2.0), make_position("ZW", -3.0)};
    const auto lines = stat_quality_lines_of([&] {
        auto r = mgr.finalize_previous_day(positions, {{"MES", 4510.0}, {"ZW", 600.0}},
                                           {{"MES", 4500.0}, {"ZW", 601.0}}, 500000.0, 0.0,
                                           LivePnLManager::UnrealizedPolicy::SETTLED, {"ZW"});
        ASSERT_TRUE(r.is_ok());
    });
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "STAT_QUALITY finalize held=2 no_bar=0");
}

// The cash book's finalize (the equity runner's policy) writes the same line.
TEST_F(LivePnLManagerTest, FinalizeOfACashBookLogsTheSameLine) {
    LivePnLManager mgr(100000.0, InstrumentRegistry::instance());
    const std::vector<Position> positions{make_position("AAPL", 10.0), make_position("TMUS", 5.0),
                                          make_position("DD", 0.0)};
    const auto lines = stat_quality_lines_of([&] {
        auto r = mgr.finalize_previous_day(positions, {{"AAPL", 201.0}},
                                           {{"AAPL", 200.0}, {"TMUS", 240.0}}, 100000.0, 0.0,
                                           LivePnLManager::UnrealizedPolicy::MARK_TO_MARKET);
        ASSERT_TRUE(r.is_ok());
    });
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "STAT_QUALITY finalize held=2 no_bar=1: TMUS");
}

// An empty book is finalized too (nothing held): one line, both counts 0. A finalize refused for
// want of any T-2 close settles nothing and writes no line.
TEST_F(LivePnLManagerTest, FinalizeOfAnEmptyBookLogsZeroAndARefusedFinalizeLogsNoLine) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    const auto empty = stat_quality_lines_of([&] {
        ASSERT_TRUE(mgr.finalize_previous_day({}, {}, {}, 500000.0, 0.0).is_ok());
    });
    ASSERT_EQ(empty.size(), 1u);
    EXPECT_EQ(empty[0], "STAT_QUALITY finalize held=0 no_bar=0");

    const auto refused = stat_quality_lines_of([&] {
        EXPECT_TRUE(mgr.finalize_previous_day({make_position("MES", 1.0)}, {{"MES", 4500.0}}, {},
                                              500000.0, 0.0)
                        .is_error());
    });
    EXPECT_TRUE(refused.empty());
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

TEST_F(LivePnLManagerTest, GetPointValueHasNoAnswerForAFutureNotInRegistry) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    // CL not registered: no guessed 1000.0, point value 0 and an ERROR (CM1 d)
    EXPECT_DOUBLE_EQ(mgr.get_point_value("CL"), 0.0);
}

TEST_F(LivePnLManagerTest, GetPointValueUnknownFuturesSymbolReturnsZero) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    EXPECT_DOUBLE_EQ(mgr.get_point_value("UNKNOWN_XYZ"), 0.0);
}

TEST_F(LivePnLManagerTest, GetPointValueStripsVariantSuffix) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    // ES.v.0 should normalize to ES → MES → 50.0
    EXPECT_DOUBLE_EQ(mgr.get_point_value("ES.v.0"), 50.0);
}

// E2-F38 / BA-7: the fallback table is matched by SUBSTRING over futures contract
// codes, so an unregistered EQUITY ticker that happens to contain one inherits a
// futures multiplier. These are all real US tickers, and the multipliers below are
// what the table actually returns for them today.
TEST_F(LivePnLManagerTest, UnregisteredEquityDoesNotInheritAFuturesMultiplier) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    mgr.set_asset_type(AssetType::EQUITY);

    // LEN is the spinoff parent in this campaign's own corp-action work (LEN/Millrose).
    EXPECT_DOUBLE_EQ(mgr.get_point_value("LEN"), 1.0) << "matches \"LE\", live cattle, 40000";
    EXPECT_DOUBLE_EQ(mgr.get_point_value("ZM"), 1.0) << "Zoom matches \"ZM\", soybean meal, 100";
    EXPECT_DOUBLE_EQ(mgr.get_point_value("UBER"), 1.0) << "matches \"UB\", ultra T-bond, 1000";
    EXPECT_DOUBLE_EQ(mgr.get_point_value("HES"), 1.0) << "Hess matches \"ES\", E-mini S&P, 5";
    EXPECT_DOUBLE_EQ(mgr.get_point_value("SIRI"), 1.0) << "matches \"SI\", silver, 5000";
    EXPECT_DOUBLE_EQ(mgr.get_point_value("PLD"), 1.0) << "Prologis matches \"PL\", platinum, 50";

    // A share is a share: every equity point value is 1.0, registered or not.
    EXPECT_DOUBLE_EQ(mgr.get_point_value("AAPL"), 1.0);
}

// A futures manager (the default) has no fallback table any more (CM1 d): a future the registry
// does not hold has no point value (0, ERROR), where the deleted table guessed, several times
// wrongly (GC 1000 for 100).
TEST_F(LivePnLManagerTest, DefaultAssetTypeHasNoFuturesFallbackTable) {
    LivePnLManager mgr(500000.0, InstrumentRegistry::instance());
    EXPECT_EQ(mgr.asset_type(), AssetType::FUTURE) << "silent callers stay on futures";
    EXPECT_DOUBLE_EQ(mgr.get_point_value("CL"), 0.0);
    EXPECT_DOUBLE_EQ(mgr.get_point_value("GC"), 0.0);
    EXPECT_DOUBLE_EQ(mgr.get_point_value("ZM"), 0.0);
    EXPECT_DOUBLE_EQ(mgr.get_point_value("UNKNOWN_XYZ"), 0.0);
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
