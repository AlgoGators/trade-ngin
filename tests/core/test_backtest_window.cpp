// Pin for M-12: the frozen backtest window.
//
// Two things have to hold and they pull against each other.
//
// 1. With `backtest.frozen_end_date` ABSENT -- which is how config_template and
//    the deployed config ship -- resolve_backtest_window must produce exactly
//    what the three bt runners produced inline before this existed:
//    end = now(), start = the same local broken-down time with tm_year reduced
//    by lookback_years and re-normalised by std::mktime. Not "about the same":
//    the same to the second, because a one-second shift moves which bars the
//    window contains, and the whole class-A claim is that a run without the key
//    is unchanged.
//
// 2. With the key SET, the window must not move when the clock does. That is
//    the property the A/B needs and the reason M-12 was opened.
//
// The first test is written as an independent RE-DERIVATION of the old inline
// arithmetic rather than a call into the helper twice, so it fails if the
// helper's default path is changed at all.

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <stdexcept>

#include "trade_ngin/core/config_loader.hpp"

using namespace trade_ngin;

namespace {

// The arithmetic apps/backtest/*.cpp used to do inline, reproduced here so the
// test is an oracle and not an echo.
std::pair<Timestamp, Timestamp> legacy_window(int lookback_years, Timestamp now) {
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    std::tm start_tm = *now_tm;
    start_tm.tm_year -= lookback_years;
    auto start_time_t = std::mktime(&start_tm);
    return {std::chrono::system_clock::from_time_t(start_time_t), now};
}

Timestamp at(int y, int m, int d, int hh, int mm, int ss) {
    std::tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_hour = hh;
    t.tm_min = mm;
    t.tm_sec = ss;
    t.tm_isdst = -1;
    return std::chrono::system_clock::from_time_t(std::mktime(&t));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Default path is byte-identical to the old inline arithmetic
// ---------------------------------------------------------------------------

TEST(BacktestWindowM12, UnsetKeyReproducesTheLegacyWindowExactly) {
    BacktestSpecificConfig cfg;  // frozen_end_date defaults to empty
    ASSERT_TRUE(cfg.frozen_end_date.empty())
        << "the frozen window must be OFF by default or every production backtest changes";

    // A spread of clocks, including a DST boundary and a leap day, because the
    // tm_year-- then mktime() dance is exactly where those bite.
    const Timestamp clocks[] = {
        at(2026, 9, 9, 6, 39, 34),
        at(2026, 3, 8, 2, 30, 0),    // US DST spring-forward morning
        at(2026, 11, 1, 1, 30, 0),   // US DST fall-back morning
        at(2028, 2, 29, 23, 59, 59), // leap day
        at(2026, 1, 1, 0, 0, 0),
    };
    for (int lookback : {1, 2, 3, 5}) {
        cfg.lookback_years = lookback;
        for (Timestamp now : clocks) {
            bool froze = true;
            auto got = ConfigLoader::resolve_backtest_window(cfg, now, &froze);
            auto want = legacy_window(lookback, now);
            EXPECT_EQ(got.first, want.first)
                << "start_date moved for lookback_years=" << lookback;
            EXPECT_EQ(got.second, want.second)
                << "end_date moved for lookback_years=" << lookback;
            EXPECT_FALSE(froze) << "the frozen flag must stay false when the key is absent";
        }
    }
}

TEST(BacktestWindowM12, UnsetKeyStillTracksTheWallClock) {
    // The complement of the test above: without the key the window MUST slide,
    // because that is what production does. A helper that froze by default
    // would pass the equality test above only if the caller passed the same
    // now() twice, so this pins the direction too.
    BacktestSpecificConfig cfg;
    auto a = ConfigLoader::resolve_backtest_window(cfg, at(2026, 9, 9, 6, 0, 0));
    auto b = ConfigLoader::resolve_backtest_window(cfg, at(2026, 9, 10, 6, 0, 0));
    EXPECT_NE(a.second, b.second);
    EXPECT_NE(a.first, b.first);
}

TEST(BacktestWindowM12, UnsetKeyIsNotSerialised) {
    // A production config round-tripped through to_json must not acquire the
    // key, or the next reader would think a frozen run was intended.
    BacktestSpecificConfig cfg;
    EXPECT_FALSE(cfg.to_json().contains("frozen_end_date"));

    BacktestSpecificConfig set;
    set.frozen_end_date = "2026-05-03";
    EXPECT_TRUE(set.to_json().contains("frozen_end_date"));
}

// ---------------------------------------------------------------------------
// 2. The frozen path does what it exists for
// ---------------------------------------------------------------------------

TEST(BacktestWindowM12, FrozenKeyPinsTheWindowAcrossDifferentClocks) {
    BacktestSpecificConfig cfg;
    cfg.lookback_years = 2;
    cfg.frozen_end_date = "2026-05-03";

    bool froze_a = false, froze_b = false;
    // Six months of wall-clock drift, which is a thousand times the ~6 h the
    // sentinel-drift memory says a line-by-line comparison survives today.
    auto a = ConfigLoader::resolve_backtest_window(cfg, at(2026, 9, 9, 6, 39, 34), &froze_a);
    auto b = ConfigLoader::resolve_backtest_window(cfg, at(2027, 3, 1, 22, 0, 0), &froze_b);

    EXPECT_EQ(a.first, b.first) << "start_date drifted with the clock despite the frozen key";
    EXPECT_EQ(a.second, b.second) << "end_date drifted with the clock despite the frozen key";
    EXPECT_TRUE(froze_a);
    EXPECT_TRUE(froze_b);

    EXPECT_EQ(a.second, at(2026, 5, 3, 0, 0, 0));
    EXPECT_EQ(a.first, at(2024, 5, 3, 0, 0, 0));
}

TEST(BacktestWindowM12, FrozenKeyHonoursLookbackYears) {
    BacktestSpecificConfig cfg;
    cfg.frozen_end_date = "2026-05-03";
    cfg.lookback_years = 1;
    auto one = ConfigLoader::resolve_backtest_window(cfg, at(2026, 9, 9, 6, 0, 0));
    EXPECT_EQ(one.first, at(2025, 5, 3, 0, 0, 0));
    cfg.lookback_years = 3;
    auto three = ConfigLoader::resolve_backtest_window(cfg, at(2026, 9, 9, 6, 0, 0));
    EXPECT_EQ(three.first, at(2023, 5, 3, 0, 0, 0));
}

TEST(BacktestWindowM12, MalformedFrozenDateThrowsRatherThanSilentlyUnfreezing) {
    // A typo that fell back to now() would put the drift back without any sign
    // in the log, which is the failure this whole item exists to remove.
    BacktestSpecificConfig cfg;
    for (const char* bad : {"03-05-2026", "2026/05/03", "not-a-date", "2026-05"}) {
        cfg.frozen_end_date = bad;
        EXPECT_THROW(ConfigLoader::resolve_backtest_window(cfg, at(2026, 9, 9, 6, 0, 0)),
                     std::runtime_error)
            << "accepted malformed frozen_end_date '" << bad << "'";
    }
}

TEST(BacktestWindowM12, TemplateConfigDoesNotShipTheKey) {
    // Belt and braces against the key reaching production by way of the
    // template everyone copies.
    BacktestSpecificConfig from_template;
    nlohmann::json j = {{"lookback_years", 2}, {"store_trade_details", true}};
    from_template.from_json(j);
    EXPECT_TRUE(from_template.frozen_end_date.empty());
}
