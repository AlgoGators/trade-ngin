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

// ===== E2-F14: caller-resolved T-1 date =====
//
// Omitted, T-1 is `reference_date - 24h` matched against the LAST bar only. That suits
// FUTURES, whose runners resolve the book they are finalizing with the same arithmetic
// (live_portfolio.cpp:1047, live_portfolio_conservative.cpp:1061) -- book and prices name
// the same day by construction.
//
// EQUITIES resolve the book with find_previous_trading_day(), which is calendar-aware. On a
// Sunday or Monday run that returns FRIDAY while `now - 24h` asks for Sat/Sun, and
// equities_data.ohlcv_1d has no weekend rows. The already-finalized Friday was then
// re-finalized against an empty price map and its position rows overwritten with 0/0 by a
// store_positions that runs outside the gate protecting live_results.
//
// Measured on the 2026-07-24..08-04 weekend-inclusive replay: Sat 08-01 finalized Friday at
// $864.759444; Sun 08-02 and Mon 08-03 each re-zeroed it. L5 residual -864.7594 on 07-31,
// -14.6574 on 07-24, 0.0000 on every other finalized day.

namespace {
// Exact UTC midnights, so floor<days> boundaries are unambiguous.
constexpr int64_t kDay = 86400;
constexpr int64_t kThu = 20665 * kDay;  // an arbitrary but fixed Thursday
constexpr int64_t kFri = kThu + kDay;
constexpr int64_t kSat = kThu + 2 * kDay;
constexpr int64_t kSun = kThu + 3 * kDay;
constexpr int64_t kMon = kThu + 4 * kDay;
}  // namespace

// The futures invariant: passing no t1_date must behave exactly as before.
TEST_F(LivePriceManagerTest, OmittingT1DateKeepsTheMinus24hRuleForFuturesCallers) {
    LivePriceManager mgr(nullptr);
    std::vector<Bar> bars{make_bar("ZC", ts_seconds(kThu), 600.0),
                          make_bar("ZC", ts_seconds(kFri), 610.0)};

    // Reference Saturday -> expects Friday -> found.
    ASSERT_TRUE(mgr.update_from_bars(bars, ts_seconds(kSat)).is_ok());
    auto t1 = mgr.get_previous_day_price("ZC");
    ASSERT_TRUE(t1.is_ok());
    EXPECT_DOUBLE_EQ(t1.value(), 610.0);

    // Reference Monday -> expects Sunday -> ag future has no Sunday bar -> skipped.
    // This is CORRECT for futures and must not become a fallback: falling back to Friday
    // here would book (Fri-Thu) onto the Sunday row, a move the Saturday run already
    // booked onto Friday's row.
    LivePriceManager mgr2(nullptr);
    ASSERT_TRUE(mgr2.update_from_bars(bars, ts_seconds(kMon)).is_ok());
    EXPECT_TRUE(mgr2.get_previous_day_price("ZC").is_error())
        << "The default path gained a T-1 fallback. Futures relies on the skip: with a "
           "fallback an ag symbol would have the same move settled twice.";
}

// The equity fix: a Monday run resolving Friday must find Friday's bar.
TEST_F(LivePriceManagerTest, CallerResolvedT1FindsFridayOnAMondayRun) {
    LivePriceManager mgr(nullptr);
    std::vector<Bar> bars{make_bar("AAPL", ts_seconds(kThu), 100.0),
                          make_bar("AAPL", ts_seconds(kFri), 105.0)};

    // now = Monday, but the book was resolved to Friday by find_previous_trading_day().
    ASSERT_TRUE(mgr.update_from_bars(bars, ts_seconds(kMon), ts_seconds(kFri)).is_ok());

    auto t1 = mgr.get_previous_day_price("AAPL");
    ASSERT_TRUE(t1.is_ok())
        << "The resolved trading day produced no T-1 price. This is the defect: the book "
           "says Friday, the price lookup asked for Sunday, and Friday's finalized rows "
           "were overwritten with zeros.";
    EXPECT_DOUBLE_EQ(t1.value(), 105.0);

    auto t2 = mgr.get_two_days_ago_price("AAPL");
    ASSERT_TRUE(t2.is_ok());
    EXPECT_DOUBLE_EQ(t2.value(), 100.0)
        << "T-2 must be the bar immediately preceding the resolved T-1 bar.";
}

// The reason the lookup SEARCHES rather than testing back(): on a live run the vendor may
// already have posted today's bar. A back()-only test would then reject a good Friday.
TEST_F(LivePriceManagerTest, CallerResolvedT1FindsTheBarEvenWhenALaterBarExists) {
    LivePriceManager mgr(nullptr);
    std::vector<Bar> bars{make_bar("MSFT", ts_seconds(kThu), 200.0),
                          make_bar("MSFT", ts_seconds(kFri), 205.0),
                          make_bar("MSFT", ts_seconds(kMon), 210.0)};  // today, already posted

    ASSERT_TRUE(mgr.update_from_bars(bars, ts_seconds(kMon), ts_seconds(kFri)).is_ok());

    auto t1 = mgr.get_previous_day_price("MSFT");
    ASSERT_TRUE(t1.is_ok())
        << "A back()-only match was reinstated: today's bar is last, so Friday was missed.";
    EXPECT_DOUBLE_EQ(t1.value(), 205.0);
    EXPECT_DOUBLE_EQ(mgr.get_two_days_ago_price("MSFT").value(), 200.0);
}

// A genuine data gap on a real trading day must still be skipped, not papered over.
TEST_F(LivePriceManagerTest, CallerResolvedT1StillSkipsWhenThatDayHasNoBar) {
    LivePriceManager mgr(nullptr);
    std::vector<Bar> bars{make_bar("ABEV", ts_seconds(kThu), 5.0)};  // nothing on Friday

    ASSERT_TRUE(mgr.update_from_bars(bars, ts_seconds(kMon), ts_seconds(kFri)).is_ok());

    EXPECT_TRUE(mgr.get_previous_day_price("ABEV").is_error())
        << "Supplying t1_date must not become a licence to substitute an older bar. A real "
           "gap on a real trading day is still a skip.";
    EXPECT_TRUE(mgr.get_latest_price("ABEV").is_ok())
        << "latest_prices_ is still populated as a fallback for other consumers.";
}

// Re-finalizing the same day must be idempotent -- that is what makes the Sun/Mon repeat
// runs harmless once the date is shared.
TEST_F(LivePriceManagerTest, CallerResolvedT1IsIdempotentAcrossRepeatedRuns) {
    std::vector<Bar> bars{make_bar("GOOGL", ts_seconds(kThu), 300.0),
                          make_bar("GOOGL", ts_seconds(kFri), 310.0)};

    double first = 0.0;
    for (int64_t ref : {kSat, kSun, kMon}) {
        LivePriceManager mgr(nullptr);
        ASSERT_TRUE(mgr.update_from_bars(bars, ts_seconds(ref), ts_seconds(kFri)).is_ok());
        auto t1 = mgr.get_previous_day_price("GOOGL");
        ASSERT_TRUE(t1.is_ok()) << "Run with reference day " << ref << " lost the T-1 price.";
        if (first == 0.0) first = t1.value();
        EXPECT_DOUBLE_EQ(t1.value(), first)
            << "Sat/Sun/Mon runs finalizing the same Friday produced different T-1 prices.";
        EXPECT_DOUBLE_EQ(t1.value(), 310.0);
    }
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
