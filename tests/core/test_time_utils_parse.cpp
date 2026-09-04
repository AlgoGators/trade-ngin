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
