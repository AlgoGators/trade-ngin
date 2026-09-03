// tests/core/test_time_utils_parse.cpp
//
// E2-F22: a timestamptz read back from a UTC session must round-trip exactly on ANY host.
// std::mktime interpreted the broken-down UTC time as host-local, so on a New York host a
// loaded position timestamp came back +5 h; the T-1 rewrite persisted the shift, and four
// rewrites (a holiday adjacent to a weekend) walked the row across midnight onto the next
// date, where the date-keyed DELETE replaced that day's rows with a copy of T-1's.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "trade_ngin/core/time_utils.hpp"

using namespace trade_ngin::core;

namespace {

struct ForceTimezone {
    std::string previous;
    bool had_previous = false;
    explicit ForceTimezone(const char* tz) {
        if (const char* p = std::getenv("TZ")) { previous = p; had_previous = true; }
        setenv("TZ", tz, 1);
        tzset();
    }
    ~ForceTimezone() {
        if (had_previous) setenv("TZ", previous.c_str(), 1); else unsetenv("TZ");
        tzset();
    }
};

}  // namespace

// The load-bearing case: the host is New York, the string is UTC.
TEST(TimeUtilsParse, UtcStringRoundTripsExactlyOnANewYorkHost) {
    ForceTimezone tz("America/New_York");
    std::chrono::system_clock::time_point tp;
    ASSERT_TRUE(parse_utc_datetime("2026-06-18 05:00:00+00", tp));
    EXPECT_EQ(format_utc_datetime(tp), "2026-06-18 05:00:00")
        << "a UTC timestamp must not pick up the host offset (mktime did: +5 h)";
}

// Fractional seconds and the +00 suffix pqxx hands back are tolerated.
TEST(TimeUtilsParse, ToleratesFractionAndOffsetSuffix) {
    ForceTimezone tz("America/New_York");
    std::chrono::system_clock::time_point tp;
    ASSERT_TRUE(parse_utc_datetime("2026-07-02 20:00:00.123456+00", tp));
    EXPECT_EQ(format_utc_date(tp), "2026-07-02");
    EXPECT_EQ(format_utc_datetime(tp), "2026-07-02 20:00:00");
}

// Four re-parses of the same string stay on the same date -- the migration that produced
// the duplicate Juneteenth row cannot start.
TEST(TimeUtilsParse, RepeatedParseFormatIsAFixedPoint) {
    ForceTimezone tz("America/New_York");
    std::string s = "2026-07-02 20:00:00+00";
    for (int i = 0; i < 4; ++i) {
        std::chrono::system_clock::time_point tp;
        ASSERT_TRUE(parse_utc_datetime(s, tp));
        s = format_utc_datetime(tp) + "+00";
    }
    EXPECT_EQ(s, "2026-07-02 20:00:00+00");
}

TEST(TimeUtilsParse, MalformedStringIsRejected) {
    std::chrono::system_clock::time_point tp;
    EXPECT_FALSE(parse_utc_datetime("not a timestamp", tp));
    EXPECT_FALSE(parse_utc_datetime("2026-07-02", tp));
}

// ──────────────────────────────────────────────────────────────────────────
// BA-12 / C-1 C8 -- the data-freshness guard.
//
// It used to take the GLOBAL MAX bar timestamp across every loaded bar, so a
// single symbol printing today reported the whole feed as current no matter how
// far behind the other 851 were. It also treated an empty load as "nothing to
// check" and measured staleness by instant subtraction rather than calendar
// days between UTC date keys.
//
// Lives in this file because all three defects are UTC/calendar-date defects and
// ForceTimezone above is what proves the last of them.
// ──────────────────────────────────────────────────────────────────────────

#include "trade_ngin/live/data_freshness.hpp"

using trade_ngin::assess_feed_freshness;
using trade_ngin::calendar_days_between_utc;

TEST(FeedFreshness, OneFreshSymbolDoesNotMaskTheStaleRest) {
    // The universe-scale failure in one fixture: AAPL printed today, everything
    // else stopped in June. A max would report 0 days behind.
    const std::unordered_map<std::string, std::string> last_bar{
        {"AAPL", "2026-08-06"},
        {"TMUS", "2026-06-02"},
        {"NSC", "2026-06-15"},
        {"ABT", "2026-07-01"},
    };

    const auto f = assess_feed_freshness(last_bar, "2026-08-06");

    ASSERT_TRUE(f.any_data);
    EXPECT_EQ(f.stalest_symbol, "TMUS") << "the STALEST symbol decides, not the freshest";
    EXPECT_EQ(f.stalest_date, "2026-06-02");
    EXPECT_EQ(f.days_behind, 65) << "2026-06-02 to 2026-08-06 is 65 calendar days";
    EXPECT_EQ(f.symbols, 4u);
}

TEST(FeedFreshness, NoDataIsMaximallyStaleNotNeutral) {
    const auto f = assess_feed_freshness({}, "2026-08-06");
    EXPECT_FALSE(f.any_data)
        << "an empty load establishes that NO symbol is current; the caller must not "
           "read that as a passing check";
    EXPECT_EQ(f.symbols, 0u);
    EXPECT_TRUE(f.stalest_symbol.empty());
}

TEST(FeedFreshness, AMapOfOnlyUnparseableDatesEstablishesNothing) {
    const std::unordered_map<std::string, std::string> last_bar{
        {"AAPL", ""}, {"TMUS", "not-a-date"}, {"NSC", "2026-13-99"}};
    const auto f = assess_feed_freshness(last_bar, "2026-08-06");
    EXPECT_FALSE(f.any_data) << "nothing in the map shows a symbol is current";
    EXPECT_EQ(f.symbols, 3u) << "the symbols are still counted, so the caller can say so";
}

TEST(FeedFreshness, AFreshFeedIsZeroDaysBehind) {
    const std::unordered_map<std::string, std::string> last_bar{
        {"AAPL", "2026-08-06"}, {"TMUS", "2026-08-06"}};
    const auto f = assess_feed_freshness(last_bar, "2026-08-06");
    ASSERT_TRUE(f.any_data);
    EXPECT_EQ(f.days_behind, 0);
}

// Calendar days, not instant arithmetic. A weekend gap is exactly 3 days from
// Friday to Monday however the hours divide.
TEST(FeedFreshness, StalenessIsCountedInCalendarDays) {
    EXPECT_EQ(calendar_days_between_utc("2026-08-07", "2026-08-10"), 3) << "Fri to Mon";
    EXPECT_EQ(calendar_days_between_utc("2026-08-10", "2026-08-10"), 0);
    EXPECT_EQ(calendar_days_between_utc("2026-02-28", "2026-03-01"), 1) << "2026 is not a leap year";
    EXPECT_EQ(calendar_days_between_utc("2024-02-28", "2024-03-01"), 2) << "2024 is";
    EXPECT_EQ(calendar_days_between_utc("2025-12-31", "2026-01-01"), 1) << "across a year end";
    // A date ahead of the as-of date reports negative rather than wrapping.
    EXPECT_EQ(calendar_days_between_utc("2026-08-10", "2026-08-07"), -3);
    // Malformed input cannot manufacture a huge staleness.
    EXPECT_EQ(calendar_days_between_utc("", "2026-08-10"), 0);
    EXPECT_EQ(calendar_days_between_utc("2026-08-10", "garbage"), 0);
}

// The reason this belongs beside the mktime tests: the count must not move with
// the host zone. On a New York host an instant-based measure shifts by 5 hours,
// which flips a boundary day.
TEST(FeedFreshness, TheCountIsTheSameOnAnyHostTimezone) {
    const std::unordered_map<std::string, std::string> last_bar{{"AAPL", "2026-06-02"}};

    long tokyo = 0, newyork = 0, utc = 0;
    { ForceTimezone tz("Asia/Tokyo");        tokyo = assess_feed_freshness(last_bar, "2026-08-06").days_behind; }
    { ForceTimezone tz("America/New_York");  newyork = assess_feed_freshness(last_bar, "2026-08-06").days_behind; }
    { ForceTimezone tz("UTC");               utc = assess_feed_freshness(last_bar, "2026-08-06").days_behind; }

    EXPECT_EQ(tokyo, 65);
    EXPECT_EQ(newyork, 65);
    EXPECT_EQ(utc, 65);
}
