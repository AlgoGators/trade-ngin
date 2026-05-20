#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "trade_ngin/core/time_utils.hpp"

using namespace trade_ngin;

// Phase 5 §5c -- pins the loader's UTC date-string contract.
//
// The data layer's `YYYY-MM-DD` keys are produced via format_utc_date and
// are UTC end-to-end. This test pins three boundary-condition timestamps:
// post-spring-forward, fall-back, and a "summer evening in UTC, previous
// calendar day in Eastern" point. All three return the UTC calendar date
// regardless of the host's local timezone or DST status, which is the
// intentional contract.

namespace {

// Construct a system_clock::time_point from a UTC epoch second.
std::chrono::system_clock::time_point at_utc_seconds(time_t epoch_s) {
    return std::chrono::system_clock::from_time_t(epoch_s);
}

}  // namespace

// 2025-03-09T04:30:00Z is the morning after US Eastern's spring-forward
// (clocks jumped from 02:00 EST to 03:00 EDT at 2025-03-09T02:00 local).
// Eastern local time at this instant is 00:30 EDT on 2025-03-09 -- same UTC
// date, but only because spring-forward already happened. format_utc_date
// is unaffected by DST.
TEST(UtcDateContract, PostSpringForwardReturnsUtcDate) {
    // 2025-03-09T04:30:00Z = epoch 1741494600
    auto tp = at_utc_seconds(1741494600);
    EXPECT_EQ(core::format_utc_date(tp), "2025-03-09");
}

// 2025-11-02T05:30:00Z is during the fall-back transition (Eastern fell
// from 02:00 EDT to 01:00 EST at 2025-11-02T02:00 local). Eastern local
// time at this instant is 01:30 EDT (or 00:30 EST depending on convention).
// format_utc_date returns the UTC calendar date.
TEST(UtcDateContract, FallBackReturnsUtcDate) {
    // 2025-11-02T05:30:00Z = epoch 1762061400
    auto tp = at_utc_seconds(1762061400);
    EXPECT_EQ(core::format_utc_date(tp), "2025-11-02");
}

// 2025-06-15T03:00:00Z falls on a calendar boundary: in US/Eastern (EDT),
// this is 2025-06-14 23:00 -- the PREVIOUS calendar day. We intentionally
// return the UTC date here. If a strategy needs market-local semantics it
// must convert at the strategy boundary, not in the loader.
TEST(UtcDateContract, UtcEveningKeepsUtcDateNotEasternPrevious) {
    // 2025-06-15T03:00:00Z = epoch 1749956400
    auto tp = at_utc_seconds(1749956400);
    EXPECT_EQ(core::format_utc_date(tp), "2025-06-15");
}

// Sanity: midnight UTC is "today", not "yesterday".
TEST(UtcDateContract, MidnightUtcIsToday) {
    // 2025-01-01T00:00:00Z = epoch 1735689600
    auto tp = at_utc_seconds(1735689600);
    EXPECT_EQ(core::format_utc_date(tp), "2025-01-01");
}

// Sanity: one second before midnight UTC is the previous day.
TEST(UtcDateContract, OneSecondBeforeMidnightIsYesterday) {
    // 2024-12-31T23:59:59Z = epoch 1735689599
    auto tp = at_utc_seconds(1735689599);
    EXPECT_EQ(core::format_utc_date(tp), "2024-12-31");
}

// Pinned shape: always 10 characters, always YYYY-MM-DD format.
TEST(UtcDateContract, OutputFormatPinned) {
    auto tp = at_utc_seconds(1700000000);  // 2023-11-14T22:13:20Z
    const std::string s = core::format_utc_date(tp);
    ASSERT_EQ(s.size(), 10u);
    EXPECT_EQ(s[4], '-');
    EXPECT_EQ(s[7], '-');
}
