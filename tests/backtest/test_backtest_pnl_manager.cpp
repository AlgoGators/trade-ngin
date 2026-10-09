#include <gtest/gtest.h>
#include <chrono>
#include <unordered_map>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_pnl_manager.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Position make_pos(const std::string& sym, double qty) {
    return Position(sym, Quantity(qty), Price(100.0), Decimal(0.0), Decimal(0.0), Timestamp{});
}

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

// Metadata rows: ES 50 and NQ 20 dollars per index point.
std::shared_ptr<FuturesInstrument> index_row(const std::string& root, double multiplier) {
    FuturesSpec s;
    s.root_symbol = root;
    s.exchange = "CME";
    s.currency = "USD";
    s.multiplier = multiplier;
    s.tick_size = 0.25;
    s.commission_per_contract = 0.0;
    s.initial_margin = 12000.0;
    s.maintenance_margin = 11000.0;
    s.weight = 1.0;
    return std::make_shared<FuturesInstrument>(root, s);
}

class BacktestPnLManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        auto& registry = InstrumentRegistry::instance();
        if (registry.has_instrument("ES")) prior_es_ = registry.get_instrument("ES");
        if (registry.has_instrument("NQ")) prior_nq_ = registry.get_instrument("NQ");
        registry.register_instrument("ES", index_row("ES", 50.0));
        registry.register_instrument("NQ", index_row("NQ", 20.0));
        pnl_ = std::make_unique<BacktestPnLManager>(1'000'000.0, registry);
        pnl_->set_debug_enabled(false);
    }
    void TearDown() override {
        if (prior_es_) InstrumentRegistry::instance().register_instrument("ES", prior_es_);
        if (prior_nq_) InstrumentRegistry::instance().register_instrument("NQ", prior_nq_);
        TestBase::TearDown();
    }
    std::unique_ptr<BacktestPnLManager> pnl_;
    std::shared_ptr<Instrument> prior_es_;
    std::shared_ptr<Instrument> prior_nq_;
};

TEST_F(BacktestPnLManagerTest, InitialPortfolioValueEqualsInitialCapital) {
    EXPECT_DOUBLE_EQ(pnl_->get_portfolio_value(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_daily_total_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(pnl_->get_cumulative_total_pnl(), 0.0);
}

TEST_F(BacktestPnLManagerTest, CalculatePositionPnLLongPosition) {
    auto r = pnl_->calculate_position_pnl("ES", 2.0, 4000.0, 4010.0);
    EXPECT_TRUE(r.valid);
    // 2 * (4010-4000) * 50 (the registry's ES row) = 1000
    EXPECT_DOUBLE_EQ(r.daily_pnl, 1000.0);
    EXPECT_DOUBLE_EQ(r.point_value, 50.0);
}

TEST_F(BacktestPnLManagerTest, CalculatePositionPnLShortPosition) {
    auto r = pnl_->calculate_position_pnl("ES", -3.0, 4010.0, 4000.0);
    // -3 * (4000-4010) * 50 = 1500 (short profits when price falls)
    EXPECT_DOUBLE_EQ(r.daily_pnl, 1500.0);
}

TEST_F(BacktestPnLManagerTest, CalculatePositionPnLZeroChange) {
    auto r = pnl_->calculate_position_pnl("ES", 5.0, 4000.0, 4000.0);
    EXPECT_DOUBLE_EQ(r.daily_pnl, 0.0);
}

TEST_F(BacktestPnLManagerTest, ExtractBaseSymbolStripsContinuousFutureSuffixes) {
    // .v. and .c. suffixes are stripped before the registry lookup
    auto r1 = pnl_->calculate_position_pnl("ES.v.0", 1.0, 100.0, 101.0);
    auto r2 = pnl_->calculate_position_pnl("ES.c.0", 1.0, 100.0, 101.0);
    auto r3 = pnl_->calculate_position_pnl("ES.v.0.c.0", 1.0, 100.0, 101.0);
    EXPECT_DOUBLE_EQ(r1.point_value, 50.0);
    EXPECT_DOUBLE_EQ(r2.point_value, 50.0);
    EXPECT_DOUBLE_EQ(r3.point_value, 50.0);
}

TEST_F(BacktestPnLManagerTest, UnregisteredSymbolBooksNoPnL) {
    // No registry row: point value 0 and an ERROR, never a guessed multiplier (CM1 d).
    auto r = pnl_->calculate_position_pnl("ZZZUNKNOWN", 1.0, 100.0, 105.0);
    EXPECT_DOUBLE_EQ(r.point_value, 0.0);
    EXPECT_DOUBLE_EQ(r.daily_pnl, 0.0);
}

// Validate that each known fallback symbol resolves to its expected multiplier.
struct FallbackCase {
    const char* symbol;
    double expected;
};

class FallbackMultiplierTest : public BacktestPnLManagerTest,
                               public ::testing::WithParamInterface<FallbackCase> {};

// Every symbol the deleted fallback table used to answer (several of its answers wrong: ZR 20
// for 2,000, the micros' 5 / 2 / 0.5 for the full-size ES / NQ / YM) now has no answer without
// its registry row (CM1 d).
TEST_P(FallbackMultiplierTest, FormerFallbackSymbolsHaveNoPointValueWithoutARegistryRow) {
    auto p = GetParam();
    ASSERT_FALSE(InstrumentRegistry::instance().has_instrument(p.symbol)) << p.symbol;
    auto r = pnl_->calculate_position_pnl(p.symbol, 1.0, 100.0, 101.0);
    EXPECT_DOUBLE_EQ(r.point_value, 0.0) << "symbol=" << p.symbol << " used to be "
                                         << p.expected;
    EXPECT_DOUBLE_EQ(r.daily_pnl, 0.0) << "symbol=" << p.symbol;
}

INSTANTIATE_TEST_SUITE_P(
    AssetCategoryFallbacks, FallbackMultiplierTest,
    ::testing::Values(
        FallbackCase{"MNQ", 2.0},
        FallbackCase{"MES", 5.0},
        FallbackCase{"YM", 0.5}, FallbackCase{"MYM", 0.5},
        FallbackCase{"RTY", 5.0}, FallbackCase{"M2K", 5.0},
        FallbackCase{"MCL", 100.0}, FallbackCase{"CL", 1000.0},
        FallbackCase{"RB", 42000.0}, FallbackCase{"NG", 10000.0},
        FallbackCase{"MGC", 100.0}, FallbackCase{"GC", 100.0},
        FallbackCase{"SIL", 1000.0}, FallbackCase{"SI", 5000.0},
        FallbackCase{"HG", 25000.0}, FallbackCase{"PL", 50.0},
        FallbackCase{"6A", 100000.0}, FallbackCase{"6C", 100000.0},
        FallbackCase{"6E", 125000.0}, FallbackCase{"6J", 12500000.0},
        FallbackCase{"6M", 500000.0}, FallbackCase{"6N", 100000.0},
        FallbackCase{"6S", 125000.0}, FallbackCase{"MSF", 125000.0},
        FallbackCase{"6B", 62500.0}, FallbackCase{"M6B", 62500.0},
        FallbackCase{"ZC", 50.0}, FallbackCase{"ZS", 50.0},
        FallbackCase{"YK", 50.0}, FallbackCase{"ZW", 50.0},
        FallbackCase{"YW", 50.0}, FallbackCase{"ZM", 100.0},
        FallbackCase{"ZL", 600.0}, FallbackCase{"ZR", 20.0},
        FallbackCase{"KE", 50.0}, FallbackCase{"GF", 500.0},
        FallbackCase{"HE", 400.0}, FallbackCase{"LE", 400.0},
        FallbackCase{"ZN", 1000.0}, FallbackCase{"ZB", 1000.0},
        FallbackCase{"ZF", 1000.0}, FallbackCase{"ZT", 2000.0},
        FallbackCase{"UB", 1000.0}, FallbackCase{"VX", 1000.0}));

TEST_F(BacktestPnLManagerTest, PreviousCloseSetGetHas) {
    EXPECT_FALSE(pnl_->has_previous_close("ES"));
    EXPECT_DOUBLE_EQ(pnl_->get_previous_close("ES"), 0.0);
    pnl_->set_previous_close("ES", 4000.0);
    EXPECT_TRUE(pnl_->has_previous_close("ES"));
    EXPECT_DOUBLE_EQ(pnl_->get_previous_close("ES"), 4000.0);
}

TEST_F(BacktestPnLManagerTest, UpdatePreviousClosesBatch) {
    pnl_->update_previous_closes({{"ES", 4000.0}, {"NQ", 15000.0}});
    EXPECT_DOUBLE_EQ(pnl_->get_previous_close("ES"), 4000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_previous_close("NQ"), 15000.0);
}

TEST_F(BacktestPnLManagerTest, DailyPnLEmptyPositionsSucceedsWithZero) {
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 5), {}, {}, 0.0);
    EXPECT_TRUE(r.success);
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 0.0);
    EXPECT_DOUBLE_EQ(r.net_daily_pnl, 0.0);
    EXPECT_EQ(r.date_str, "2026-01-05");
    EXPECT_TRUE(r.position_results.empty());
}

TEST_F(BacktestPnLManagerTest, DailyPnLZeroQuantityPositionIsSkipped) {
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 0.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4010.0}};
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, prices, 0.0);
    EXPECT_TRUE(r.position_results.empty());
}

TEST_F(BacktestPnLManagerTest, DailyPnLMissingCurrentPriceMarksInvalid) {
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 1.0)}};
    std::unordered_map<std::string, double> prices;  // no entry
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, prices, 0.0);
    ASSERT_EQ(r.position_results.size(), 1u);
    const auto& pr = r.position_results.at("ES");
    EXPECT_FALSE(pr.valid);
    EXPECT_FALSE(pr.error_message.empty());
}

TEST_F(BacktestPnLManagerTest, FirstDayWithoutPreviousCloseReportsZeroPnL) {
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 2.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, prices, 0.0);
    ASSERT_EQ(r.position_results.size(), 1u);
    const auto& pr = r.position_results.at("ES");
    EXPECT_TRUE(pr.valid);
    EXPECT_DOUBLE_EQ(pr.daily_pnl, 0.0);
    // Sets previous close so the next day computes a real PnL.
    EXPECT_TRUE(pnl_->has_previous_close("ES"));
    EXPECT_DOUBLE_EQ(pnl_->get_previous_close("ES"), 4000.0);
}

TEST_F(BacktestPnLManagerTest, SecondDayComputesPnLFromStoredPreviousClose) {
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 2.0)}};
    pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, {{"ES", 4000.0}}, 0.0);
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 6), pos, {{"ES", 4010.0}}, 0.0);
    // 2 * (4010 - 4000) * 50 = 1000
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 1000.0);
    EXPECT_DOUBLE_EQ(r.net_daily_pnl, 1000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_position_daily_pnl("ES"), 1000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_position_cumulative_pnl("ES"), 1000.0);
}

TEST_F(BacktestPnLManagerTest, DailyPnLCommissionsReduceNet) {
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 2.0)}};
    pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, {{"ES", 4000.0}}, 0.0);
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 6), pos, {{"ES", 4010.0}}, 25.0);
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 1000.0);
    EXPECT_DOUBLE_EQ(r.net_daily_pnl, 975.0);
    EXPECT_DOUBLE_EQ(r.new_portfolio_value, 1'000'000.0 + 975.0);
}

TEST_F(BacktestPnLManagerTest, CumulativePnLAccumulatesAcrossDaysWithUpdates) {
    // Caller is responsible for calling update_previous_closes after each day;
    // calculate_daily_pnl only auto-stores on day 1 (no prior close).
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 1.0)}};
    pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, {{"ES", 4000.0}}, 0.0);  // day 1: 0
    pnl_->update_previous_closes({{"ES", 4000.0}});
    pnl_->calculate_daily_pnl(date_at(2026, 1, 6), pos, {{"ES", 4010.0}}, 0.0);  // +500
    pnl_->update_previous_closes({{"ES", 4010.0}});
    pnl_->calculate_daily_pnl(date_at(2026, 1, 7), pos, {{"ES", 4020.0}}, 0.0);  // +500
    EXPECT_DOUBLE_EQ(pnl_->get_position_cumulative_pnl("ES"), 1000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_cumulative_total_pnl(), 1000.0);
}

TEST_F(BacktestPnLManagerTest, MultiplePositionsAggregateCorrectly) {
    std::unordered_map<std::string, Position> pos = {
        {"ES", make_pos("ES", 1.0)},
        {"NQ", make_pos("NQ", -1.0)},
    };
    pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos,
                              {{"ES", 4000.0}, {"NQ", 15000.0}}, 0.0);
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 6), pos,
                                        {{"ES", 4010.0}, {"NQ", 14990.0}}, 0.0);
    // ES: 1 * 10 * 50 = 500; NQ: -1 * -10 * 20 = 200
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 700.0);
}

TEST_F(BacktestPnLManagerTest, ResetClearsAllStateBackToInitialCapital) {
    pnl_->set_previous_close("ES", 4000.0);
    pnl_->calculate_daily_pnl(
        date_at(2026, 1, 5),
        {{"ES", make_pos("ES", 2.0)}}, {{"ES", 4010.0}}, 0.0);
    ASSERT_NE(pnl_->get_cumulative_total_pnl(), 0.0);

    pnl_->reset();
    EXPECT_DOUBLE_EQ(pnl_->get_portfolio_value(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_cumulative_total_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(pnl_->get_daily_total_pnl(), 0.0);
    EXPECT_FALSE(pnl_->has_previous_close("ES"));
    EXPECT_DOUBLE_EQ(pnl_->get_position_daily_pnl("ES"), 0.0);
    EXPECT_DOUBLE_EQ(pnl_->get_position_cumulative_pnl("ES"), 0.0);
    EXPECT_TRUE(pnl_->get_current_date().empty());
}

TEST_F(BacktestPnLManagerTest, ResetDailyOnlyClearsDailyTracking) {
    pnl_->set_previous_close("ES", 4000.0);
    pnl_->calculate_daily_pnl(
        date_at(2026, 1, 5),
        {{"ES", make_pos("ES", 2.0)}}, {{"ES", 4010.0}}, 0.0);
    ASSERT_NE(pnl_->get_daily_total_pnl(), 0.0);
    double cum_before = pnl_->get_cumulative_total_pnl();
    double pv_before = pnl_->get_portfolio_value();

    pnl_->reset_daily();
    EXPECT_DOUBLE_EQ(pnl_->get_daily_total_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(pnl_->get_cumulative_total_pnl(), cum_before);  // preserved
    EXPECT_DOUBLE_EQ(pnl_->get_portfolio_value(), pv_before);        // preserved
    EXPECT_TRUE(pnl_->has_previous_close("ES"));                      // preserved
}

TEST_F(BacktestPnLManagerTest, SetPortfolioValueOverridesCachedValue) {
    pnl_->set_portfolio_value(2'500'000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_portfolio_value(), 2'500'000.0);
}

TEST_F(BacktestPnLManagerTest, GetCurrentDateReflectsLastCalculation) {
    pnl_->calculate_daily_pnl(date_at(2026, 3, 15), {}, {}, 0.0);
    EXPECT_EQ(pnl_->get_current_date(), "2026-03-15");
}
