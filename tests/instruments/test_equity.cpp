#include <gtest/gtest.h>
#include <chrono>
#include "../core/test_base.hpp"
#include "trade_ngin/instruments/equity.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

EquitySpec make_spec() {
    EquitySpec s;
    s.exchange = "NASDAQ";
    s.currency = "USD";
    s.lot_size = 100.0;
    s.tick_size = 0.01;
    s.commission_per_share = 0.005;
    s.is_etf = false;
    s.is_marginable = true;
    s.margin_requirement = 0.5;
    s.sector = "Technology";
    s.industry = "Software";
    s.trading_hours = "09:30-16:00";
    return s;
}

}  // namespace

class EquityInstrumentTest : public TestBase {};

TEST_F(EquityInstrumentTest, ConstructorAndAccessors) {
    auto spec = make_spec();
    EquityInstrument eq("AAPL", spec);
    EXPECT_EQ(eq.get_symbol(), "AAPL");
    EXPECT_EQ(eq.get_type(), AssetType::EQUITY);
    EXPECT_EQ(eq.get_exchange(), "NASDAQ");
    EXPECT_EQ(eq.get_currency(), "USD");
    EXPECT_DOUBLE_EQ(eq.get_multiplier(), 1.0);  // hard-coded for stocks
    EXPECT_DOUBLE_EQ(eq.get_tick_size(), 0.01);
    EXPECT_DOUBLE_EQ(eq.get_commission_per_contract(), 0.005 * 100.0);
    EXPECT_DOUBLE_EQ(eq.get_point_value(), 1.0);
    EXPECT_DOUBLE_EQ(eq.get_margin_requirement(), 0.5);
    EXPECT_EQ(eq.get_trading_hours(), "09:30-16:00");
    EXPECT_DOUBLE_EQ(eq.get_lot_size(), 100.0);
    EXPECT_FALSE(eq.is_etf());
    EXPECT_TRUE(eq.is_marginable());
    EXPECT_EQ(eq.get_sector(), "Technology");
    EXPECT_EQ(eq.get_industry(), "Software");
    EXPECT_TRUE(eq.get_dividends().empty());
}

TEST_F(EquityInstrumentTest, IsTradeableTrueWithValidConfig) {
    EquityInstrument eq("AAPL", make_spec());
    EXPECT_TRUE(eq.is_tradeable());
}

TEST_F(EquityInstrumentTest, IsTradeableFalseOnEmptySymbol) {
    EquityInstrument eq("", make_spec());
    EXPECT_FALSE(eq.is_tradeable());
}

TEST_F(EquityInstrumentTest, IsTradeableFalseOnEmptyExchange) {
    auto spec = make_spec();
    spec.exchange = "";
    EquityInstrument eq("AAPL", spec);
    EXPECT_FALSE(eq.is_tradeable());
}

TEST_F(EquityInstrumentTest, IsTradeableFalseOnNonPositiveTickSize) {
    auto spec = make_spec();
    spec.tick_size = 0.0;
    EquityInstrument eq("AAPL", spec);
    EXPECT_FALSE(eq.is_tradeable());

    spec.tick_size = -0.01;
    EquityInstrument eq2("AAPL", spec);
    EXPECT_FALSE(eq2.is_tradeable());
}

TEST_F(EquityInstrumentTest, IsTradeableFalseOnNonPositiveLotSize) {
    auto spec = make_spec();
    spec.lot_size = 0.0;
    EquityInstrument eq("AAPL", spec);
    EXPECT_FALSE(eq.is_tradeable());
}

TEST_F(EquityInstrumentTest, RoundPriceSnapsToNearestTick) {
    EquityInstrument eq("AAPL", make_spec());
    EXPECT_DOUBLE_EQ(eq.round_price(150.123), 150.12);
    EXPECT_DOUBLE_EQ(eq.round_price(150.126), 150.13);
}

TEST_F(EquityInstrumentTest, GetNotionalValueIsQuantityTimesPrice) {
    EquityInstrument eq("AAPL", make_spec());
    EXPECT_DOUBLE_EQ(eq.get_notional_value(100.0, 150.0), 15000.0);
    EXPECT_DOUBLE_EQ(eq.get_notional_value(-100.0, 150.0), 15000.0);
}

TEST_F(EquityInstrumentTest, CalculateCommissionIsQuantityTimesPerShare) {
    EquityInstrument eq("AAPL", make_spec());
    EXPECT_DOUBLE_EQ(eq.calculate_commission(1000.0), 5.0);
    EXPECT_DOUBLE_EQ(eq.calculate_commission(-1000.0), 5.0);
}

TEST_F(EquityInstrumentTest, IsMarketOpenFalseOnWeekend) {
    EquityInstrument eq("AAPL", make_spec());
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 10;  // Saturday
    tm.tm_hour = 12;
    auto sat = std::chrono::system_clock::from_time_t(timegm(&tm));
    EXPECT_FALSE(eq.is_market_open(sat));
}

TEST_F(EquityInstrumentTest, IsMarketOpenFalseOnMalformedTradingHours) {
    auto spec = make_spec();
    spec.trading_hours = "garbage";
    EquityInstrument eq("AAPL", spec);
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 7;  // Wednesday
    tm.tm_hour = 12;
    auto ts = std::chrono::system_clock::from_time_t(timegm(&tm));
    EXPECT_FALSE(eq.is_market_open(ts));
}

TEST_F(EquityInstrumentTest, GetNextDividendNoDividendsReturnsNullopt) {
    EquityInstrument eq("AAPL", make_spec());
    EXPECT_FALSE(eq.get_next_dividend(std::chrono::system_clock::now()).has_value());
}

TEST_F(EquityInstrumentTest, GetNextDividendReturnsFirstFutureEntry) {
    auto spec = make_spec();
    auto now = std::chrono::system_clock::now();
    DividendInfo d_past{now - std::chrono::hours(24 * 30),
                         now - std::chrono::hours(24 * 25),
                         0.5, false};
    DividendInfo d_future{now + std::chrono::hours(24 * 30),
                           now + std::chrono::hours(24 * 35),
                           0.6, false};
    DividendInfo d_far{now + std::chrono::hours(24 * 90),
                        now + std::chrono::hours(24 * 95),
                        0.7, true};
    spec.dividends = {d_past, d_future, d_far};
    EquityInstrument eq("AAPL", spec);
    auto next = eq.get_next_dividend(now);
    ASSERT_TRUE(next.has_value());
    EXPECT_DOUBLE_EQ(next->amount, 0.6);
    EXPECT_FALSE(next->is_special);
}

TEST_F(EquityInstrumentTest, GetNextDividendReturnsNulloptWhenAllPast) {
    auto spec = make_spec();
    auto now = std::chrono::system_clock::now();
    DividendInfo d_past{now - std::chrono::hours(24 * 5),
                         now - std::chrono::hours(24 * 1),
                         0.5, false};
    spec.dividends = {d_past};
    EquityInstrument eq("AAPL", spec);
    EXPECT_FALSE(eq.get_next_dividend(now).has_value());
}

TEST_F(EquityInstrumentTest, ETFFlagPropagates) {
    auto spec = make_spec();
    spec.is_etf = true;
    EquityInstrument eq("SPY", spec);
    EXPECT_TRUE(eq.is_etf());
}
