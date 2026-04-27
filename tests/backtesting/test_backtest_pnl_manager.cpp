#include <gtest/gtest.h>
#include <chrono>
#include <unordered_map>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_pnl_manager.hpp"
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

class BacktestPnLManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        pnl_ = std::make_unique<BacktestPnLManager>(1'000'000.0, InstrumentRegistry::instance());
        pnl_->set_debug_enabled(false);
    }
    std::unique_ptr<BacktestPnLManager> pnl_;
};

TEST_F(BacktestPnLManagerTest, InitialPortfolioValueEqualsInitialCapital) {
    EXPECT_DOUBLE_EQ(pnl_->get_portfolio_value(), 1'000'000.0);
    EXPECT_DOUBLE_EQ(pnl_->get_daily_total_pnl(), 0.0);
    EXPECT_DOUBLE_EQ(pnl_->get_cumulative_total_pnl(), 0.0);
}

TEST_F(BacktestPnLManagerTest, CalculatePositionPnLLongPosition) {
    auto r = pnl_->calculate_position_pnl("ES", 2.0, 4000.0, 4010.0);
    EXPECT_TRUE(r.valid);
    // 2 * (4010-4000) * 5 (ES point value fallback) = 100
    EXPECT_DOUBLE_EQ(r.daily_pnl, 100.0);
    EXPECT_DOUBLE_EQ(r.point_value, 5.0);
}

TEST_F(BacktestPnLManagerTest, CalculatePositionPnLShortPosition) {
    auto r = pnl_->calculate_position_pnl("ES", -3.0, 4010.0, 4000.0);
    // -3 * (4000-4010) * 5 = 150 (short profits when price falls)
    EXPECT_DOUBLE_EQ(r.daily_pnl, 150.0);
}

TEST_F(BacktestPnLManagerTest, CalculatePositionPnLZeroChange) {
    auto r = pnl_->calculate_position_pnl("ES", 5.0, 4000.0, 4000.0);
    EXPECT_DOUBLE_EQ(r.daily_pnl, 0.0);
}

TEST_F(BacktestPnLManagerTest, ExtractBaseSymbolStripsContinuousFutureSuffixes) {
    // .v. and .c. suffixes are stripped before fallback lookup
    auto r1 = pnl_->calculate_position_pnl("ES.v.0", 1.0, 100.0, 101.0);
    auto r2 = pnl_->calculate_position_pnl("ES.c.0", 1.0, 100.0, 101.0);
    auto r3 = pnl_->calculate_position_pnl("ES.v.0.c.0", 1.0, 100.0, 101.0);
    EXPECT_DOUBLE_EQ(r1.point_value, 5.0);
    EXPECT_DOUBLE_EQ(r2.point_value, 5.0);
    EXPECT_DOUBLE_EQ(r3.point_value, 5.0);
}

TEST_F(BacktestPnLManagerTest, FallbackMultiplierUnknownSymbolDefaultsToOne) {
    // get_fallback_multiplier returns 0 for unknown → calculate_position_pnl uses 1.0
    auto r = pnl_->calculate_position_pnl("ZZZUNKNOWN", 1.0, 100.0, 105.0);
    EXPECT_DOUBLE_EQ(r.point_value, 1.0);
    EXPECT_DOUBLE_EQ(r.daily_pnl, 5.0);
}

// Validate that each known fallback symbol resolves to its expected multiplier.
struct FallbackCase {
    const char* symbol;
    double expected;
};

class FallbackMultiplierTest : public BacktestPnLManagerTest,
                               public ::testing::WithParamInterface<FallbackCase> {};

TEST_P(FallbackMultiplierTest, FallbackPointValuesByCategory) {
    auto p = GetParam();
    auto r = pnl_->calculate_position_pnl(p.symbol, 1.0, 100.0, 100.0);
    EXPECT_DOUBLE_EQ(r.point_value, p.expected) << "symbol=" << p.symbol;
}

INSTANTIATE_TEST_SUITE_P(
    AssetCategoryFallbacks, FallbackMultiplierTest,
    ::testing::Values(
        FallbackCase{"NQ", 2.0}, FallbackCase{"MNQ", 2.0},
        FallbackCase{"ES", 5.0}, FallbackCase{"MES", 5.0},
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
    // 2 * (4010 - 4000) * 5 = 100
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 100.0);
    EXPECT_DOUBLE_EQ(r.net_daily_pnl, 100.0);
    EXPECT_DOUBLE_EQ(pnl_->get_position_daily_pnl("ES"), 100.0);
    EXPECT_DOUBLE_EQ(pnl_->get_position_cumulative_pnl("ES"), 100.0);
}

TEST_F(BacktestPnLManagerTest, DailyPnLCommissionsReduceNet) {
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 2.0)}};
    pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, {{"ES", 4000.0}}, 0.0);
    auto r = pnl_->calculate_daily_pnl(date_at(2026, 1, 6), pos, {{"ES", 4010.0}}, 25.0);
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 100.0);
    EXPECT_DOUBLE_EQ(r.net_daily_pnl, 75.0);
    EXPECT_DOUBLE_EQ(r.new_portfolio_value, 1'000'000.0 + 75.0);
}

TEST_F(BacktestPnLManagerTest, CumulativePnLAccumulatesAcrossDaysWithUpdates) {
    // Caller is responsible for calling update_previous_closes after each day;
    // calculate_daily_pnl only auto-stores on day 1 (no prior close).
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 1.0)}};
    pnl_->calculate_daily_pnl(date_at(2026, 1, 5), pos, {{"ES", 4000.0}}, 0.0);  // day 1: 0
    pnl_->update_previous_closes({{"ES", 4000.0}});
    pnl_->calculate_daily_pnl(date_at(2026, 1, 6), pos, {{"ES", 4010.0}}, 0.0);  // +50
    pnl_->update_previous_closes({{"ES", 4010.0}});
    pnl_->calculate_daily_pnl(date_at(2026, 1, 7), pos, {{"ES", 4020.0}}, 0.0);  // +50
    EXPECT_DOUBLE_EQ(pnl_->get_position_cumulative_pnl("ES"), 100.0);
    EXPECT_DOUBLE_EQ(pnl_->get_cumulative_total_pnl(), 100.0);
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
    // ES: 1 * 10 * 5 = 50; NQ: -1 * -10 * 2 = 20
    EXPECT_DOUBLE_EQ(r.total_daily_pnl, 70.0);
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
