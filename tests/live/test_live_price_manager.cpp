// Coverage for live_price_manager.cpp. Most methods either don't touch the
// database (update_from_bars, get_*_price, clear_caches) or have stub bodies
// that don't dereference db_ (load_close_prices). We can pass nullptr for the
// DB pointer and exercise everything that matters.

#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include "trade_ngin/live/live_price_manager.hpp"

using namespace trade_ngin;

namespace {

Timestamp ts_seconds(int64_t s) {
    return std::chrono::system_clock::time_point(std::chrono::seconds(s));
}

Bar make_bar(const std::string& symbol, Timestamp t, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = t;
    b.open = Decimal(close);
    b.high = Decimal(close);
    b.low = Decimal(close);
    b.close = Decimal(close);
    b.volume = 100.0;
    return b;
}

}  // namespace

class LivePriceManagerTest : public ::testing::Test {};

// ===== load_close_prices stub returns empty for empty symbol set =====

TEST_F(LivePriceManagerTest, LoadCloseEmptySymbolsReturnsEmpty) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.load_close_prices({}, ts_seconds(0));
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

TEST_F(LivePriceManagerTest, LoadCloseNonEmptySymbolsReturnsEmptyStub) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.load_close_prices({"ES", "CL"}, ts_seconds(0));
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}

// ===== load_previous_day_prices / load_two_days_ago_prices populate caches =====

TEST_F(LivePriceManagerTest, LoadPreviousDayPricesPopulatesCacheEvenWhenStubReturnsEmpty) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.load_previous_day_prices({"ES"}, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(mgr.get_all_previous_day_prices().empty());
}

TEST_F(LivePriceManagerTest, LoadTwoDaysAgoPricesPopulatesCacheEvenWhenStubReturnsEmpty) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.load_two_days_ago_prices({"ES"}, std::chrono::system_clock::now());
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(mgr.get_all_two_days_ago_prices().empty());
}

// ===== update_from_bars =====

TEST_F(LivePriceManagerTest, UpdateFromBarsT1MatchYesterdayUpdatesPreviousDayCache) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto yesterday = today - std::chrono::hours(24);
    std::vector<Bar> bars{make_bar("ES", yesterday, 4500.0)};
    auto r = mgr.update_from_bars(bars, today);
    ASSERT_TRUE(r.is_ok());
    auto p = mgr.get_previous_day_price("ES");
    ASSERT_TRUE(p.is_ok());
    EXPECT_DOUBLE_EQ(p.value(), 4500.0);
}

TEST_F(LivePriceManagerTest, UpdateFromBarsTwoBarsPopulatesT2Cache) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto yesterday = today - std::chrono::hours(24);
    auto two_days_ago = today - std::chrono::hours(48);
    std::vector<Bar> bars{
        make_bar("ES", two_days_ago, 4490.0),
        make_bar("ES", yesterday, 4500.0),
    };
    auto r = mgr.update_from_bars(bars, today);
    ASSERT_TRUE(r.is_ok());
    auto t1 = mgr.get_previous_day_price("ES");
    auto t2 = mgr.get_two_days_ago_price("ES");
    ASSERT_TRUE(t1.is_ok());
    ASSERT_TRUE(t2.is_ok());
    EXPECT_DOUBLE_EQ(t1.value(), 4500.0);
    EXPECT_DOUBLE_EQ(t2.value(), 4490.0);
}

TEST_F(LivePriceManagerTest, UpdateFromBarsLastBarNotFromYesterdaySkipsT1) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto three_days_ago = today - std::chrono::hours(72);
    std::vector<Bar> bars{make_bar("ZC", three_days_ago, 600.0)};
    auto r = mgr.update_from_bars(bars, today);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(mgr.get_previous_day_price("ZC").is_error());
    // But latest cache is still set as fallback for get_latest_price.
    EXPECT_TRUE(mgr.get_latest_price("ZC").is_ok());
}

// ===== get_*_price miss paths =====

TEST_F(LivePriceManagerTest, GetPreviousDayPriceMissingSymbolReturnsError) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.get_previous_day_price("UNKNOWN");
    EXPECT_TRUE(r.is_error());
}

TEST_F(LivePriceManagerTest, GetTwoDaysAgoPriceMissingSymbolReturnsError) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.get_two_days_ago_price("UNKNOWN");
    EXPECT_TRUE(r.is_error());
}

TEST_F(LivePriceManagerTest, GetLatestPriceFallsBackToPreviousDay) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto yesterday = today - std::chrono::hours(24);
    mgr.update_from_bars({make_bar("ES", yesterday, 4500.0)}, today);
    auto r = mgr.get_latest_price("ES");
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(r.value(), 4500.0);
}

TEST_F(LivePriceManagerTest, GetSettlementPriceMissingReturnsError) {
    LivePriceManager mgr(nullptr);
    auto r = mgr.get_settlement_price("ES", ts_seconds(0));
    EXPECT_TRUE(r.is_error());
}

// ===== Base-interface get_price / get_prices =====

TEST_F(LivePriceManagerTest, GetPriceBaseInterfaceUsesLatestCache) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto yesterday = today - std::chrono::hours(24);
    mgr.update_from_bars({make_bar("ES", yesterday, 4500.0)}, today);
    auto r = mgr.get_price("ES", today);
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(r.value(), 4500.0);
}

TEST_F(LivePriceManagerTest, GetPricesReturnsOnlyKnownSymbols) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto yesterday = today - std::chrono::hours(24);
    mgr.update_from_bars({make_bar("ES", yesterday, 4500.0)}, today);
    auto r = mgr.get_prices({"ES", "UNKNOWN"}, today);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().size(), 1u);
    EXPECT_TRUE(r.value().count("ES"));
}

// ===== clear_caches =====

TEST_F(LivePriceManagerTest, ClearCachesRemovesAllStoredPrices) {
    LivePriceManager mgr(nullptr);
    auto today = std::chrono::system_clock::now();
    auto yesterday = today - std::chrono::hours(24);
    mgr.update_from_bars({make_bar("ES", yesterday, 4500.0)}, today);
    EXPECT_FALSE(mgr.get_all_previous_day_prices().empty());
    mgr.clear_caches();
    EXPECT_TRUE(mgr.get_all_previous_day_prices().empty());
    EXPECT_TRUE(mgr.get_all_two_days_ago_prices().empty());
    EXPECT_TRUE(mgr.get_latest_price("ES").is_error());
}
