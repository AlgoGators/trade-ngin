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
#include "trade_ngin/core/logger.hpp"

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

// ===== G-03: lookback_years is checked against the enabled strategies' minimum =====
//
// config_template/defaults.json carried the coupling as a COMMENT -- "Must match
// backtest.lookback_years (2 yrs = 730 days). Strategy needs 256+ trading days
// for longest EMA and 252 for vol_lookback_long" -- and nothing enforced it. A
// short window does not fail: the longest EMA never warms up and emits a signal
// that looks exactly like a real one.
//
// validate_config now WARNs, deriving the requirement from the ema_windows of
// the strategies the config actually ENABLES. Hardcoding one number would be
// wrong in both directions, because the strategies disagree:
//
//   TrendFollowingFastConfig   longest EMA  64
//   TrendFollowingConfig       longest EMA 256
//   TrendFollowingSlowConfig   longest EMA 512   <-- {128, 512}
//
// FINDING: the template's "256+" note is understated. A book enabling
// TREND_FOLLOWING_SLOW needs 512 trading days, and lookback_years=2 gives about
// 504 -- eight short. No shipped book enables Slow today (the conservative
// portfolio enables TREND_FOLLOWING alone, longest EMA 256), so nothing is
// currently affected, which is exactly why an unchecked coupling was able to
// drift this far. Reported rather than changed: raising lookback_years alters
// every backtest window and is not a byte-identical change.
//
// Warn, not refuse, because refusing would abort runs that work today.

#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace {

int longest_ema(const std::vector<std::pair<int, int>>& windows) {
    int longest = 0;
    for (const auto& w : windows) longest = std::max(longest, w.second);
    return longest;
}

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

// The three strategies really do disagree, which is why the check is derived
// from the config rather than fixed. If they ever converge, the derivation is
// still correct -- but the comment above stops being true, so pin the numbers.
TEST(LookbackValidationG03, TheTrendStrategiesDisagreeOnTheirLongestEmaWindow) {
    EXPECT_EQ(longest_ema(TrendFollowingFastConfig{}.ema_windows), 64);
    EXPECT_EQ(longest_ema(TrendFollowingConfig{}.ema_windows), 256);
    EXPECT_EQ(longest_ema(TrendFollowingSlowConfig{}.ema_windows), 512)
        << "the slow strategy no longer needs 512 trading days; the G-03 finding about "
           "the template's understated 256+ note may be stale";
}

// The check must actually FIRE, through ConfigLoader::validate_config itself.
//
// The first version of this test reimplemented the derivation as a local lambda
// and asserted on that, so deleting the whole G-03 block from
// src/core/config_loader.cpp left it green. It proved the test could do
// arithmetic, not that the shipped code does anything. This one drives the real
// entry point and reads the real log line.
namespace {

// validate_config is private, so the test drives the PUBLIC entry point --
// ConfigLoader::load() -- against a temporary config tree. That is what every
// runner calls, so the check is exercised exactly as production exercises it.
// G-03 WARNs, so the observable is the log; console destination puts it on
// std::cout (logger.cpp, write_to_console_unsafe).
struct TempConfigTree {
    std::filesystem::path root;
    explicit TempConfigTree(const nlohmann::json& strategies, int lookback_years,
                            int historical_days) {
        root = std::filesystem::temp_directory_path() /
               ("tn_g03_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(root / "portfolios" / "probe");
        nlohmann::json defaults = {
            {"database", {{"host", "h"}, {"port", "5432"}, {"username", "u"},
                          {"password", "p"}, {"name", "n"}, {"num_connections", 2}}},
            {"backtest", {{"lookback_years", lookback_years}, {"store_trade_details", true}}},
            {"live", {{"historical_days", historical_days}}},
        };
        std::ofstream(root / "defaults.json") << defaults.dump(2);
        nlohmann::json portfolio = {
            {"portfolio_id", "G03_TEST_PORTFOLIO"},
            {"initial_capital", 500000.0},
            {"reserve_capital_pct", 0.1},
            {"strategies", strategies},
        };
        std::ofstream(root / "portfolios" / "probe" / "portfolio.json") << portfolio.dump(2);
        std::ofstream(root / "portfolios" / "probe" / "risk.json") << nlohmann::json::object().dump();
        std::ofstream(root / "portfolios" / "probe" / "email.json") << nlohmann::json::object().dump();
    }
    ~TempConfigTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

std::string load_and_capture(const TempConfigTree& tree, bool* ok) {
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::WARNING;
    Logger::instance().initialize(lc);
    ::testing::internal::CaptureStdout();
    auto r = ConfigLoader::load(tree.root, "probe");
    if (ok) *ok = r.is_ok();
    return ::testing::internal::GetCapturedStdout();
}

const nlohmann::json kTrendOnly = {
    {"TREND_FOLLOWING",
     {{"enabled_live", true}, {"config", {{"ema_windows", {{2, 8}, {64, 256}}}}}}}};

const nlohmann::json kWithSlow = {
    {"TREND_FOLLOWING",
     {{"enabled_live", true}, {"config", {{"ema_windows", {{64, 256}}}}}}},
    {"TREND_FOLLOWING_SLOW",
     {{"enabled_live", true}, {"config", {{"ema_windows", {{128, 512}}}}}}}};

const nlohmann::json kSlowDisabled = {
    {"TREND_FOLLOWING",
     {{"enabled_live", true}, {"config", {{"ema_windows", {{64, 256}}}}}}},
    {"TREND_FOLLOWING_SLOW",
     {{"enabled_live", false},
      {"enabled_backtest", false},
      {"config", {{"ema_windows", {{128, 512}}}}}}}};

}  // namespace

TEST(LookbackValidationG03, AShortWindowWarnsThroughValidateConfig) {
    bool ok = false;
    // 1 year = 252 trading days, short of TREND_FOLLOWING's 256.
    const auto out = load_and_capture(TempConfigTree(kTrendOnly, 1, 730), &ok);
    EXPECT_TRUE(ok) << "G-03 must WARN, never refuse: a short window is not a fatal config error";
    EXPECT_NE(out.find("G-03"), std::string::npos)
        << "validate_config emitted no G-03 warning for a 252-day window against a 256-day "
           "requirement. Output was:\n" << out;
    EXPECT_NE(out.find("TREND_FOLLOWING"), std::string::npos)
        << "the warning does not name the strategy that drives the requirement";
}

TEST(LookbackValidationG03, TheShippedWindowIsSilent) {
    bool ok = false;
    // What production ships: 2 years (504 days) against a 256-day requirement.
    const auto out = load_and_capture(TempConfigTree(kTrendOnly, 2, 730), &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.find("G-03"), std::string::npos)
        << "the shipped configuration now warns on every run, which would make the warning "
           "noise and get it ignored. Output was:\n" << out;
}

TEST(LookbackValidationG03, EnablingTheSlowStrategyRaisesTheRequirementAndWarns) {
    bool ok = false;
    // 2 years = 504 days, short of the slow strategy's 512.
    const auto out = load_and_capture(TempConfigTree(kWithSlow, 2, 730), &ok);
    EXPECT_TRUE(ok);
    EXPECT_NE(out.find("G-03"), std::string::npos)
        << "enabling TREND_FOLLOWING_SLOW at lookback_years=2 gives ~504 trading days against "
           "its 512-day longest EMA, and nothing warned. Output was:\n" << out;
    EXPECT_NE(out.find("512"), std::string::npos)
        << "the warning does not report the 512-day requirement the slow strategy drives";
}

TEST(LookbackValidationG03, ADisabledStrategyDoesNotRaiseTheRequirement) {
    bool ok = false;
    const auto out = load_and_capture(TempConfigTree(kSlowDisabled, 2, 730), &ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(out.find("G-03"), std::string::npos)
        << "a DISABLED strategy raised the requirement, so every book would warn about "
           "strategies it does not run. Output was:\n" << out;
}

// The shipped template must satisfy what the books it seeds actually enable.
TEST(LookbackValidationG03, TheShippedTemplateSatisfiesTheEnabledBooksRequirement) {
    const auto path = find_repo_file("config_template/defaults.json");
    ASSERT_FALSE(path.empty()) << "config_template/defaults.json not found";
    std::ifstream in(path);
    nlohmann::json tmpl;
    ASSERT_NO_THROW(in >> tmpl);

    ASSERT_TRUE(tmpl.contains("backtest"));
    const int years = tmpl.at("backtest").at("lookback_years").get<int>();
    const int trend_requirement = longest_ema(TrendFollowingConfig{}.ema_windows);
    EXPECT_GE(years * 252, trend_requirement)
        << "config_template ships lookback_years=" << years
        << ", fewer trading days than TREND_FOLLOWING's longest EMA needs (G-03)";

    ASSERT_TRUE(tmpl.contains("live"));
    const int days = tmpl.at("live").at("historical_days").get<int>();
    EXPECT_GE(static_cast<int>(days * 252 / 365.0), trend_requirement)
        << "config_template ships live.historical_days=" << days
        << ", fewer trading days than TREND_FOLLOWING's longest EMA needs (G-03)";
}

// ===== M-12: the APP half. =====
//
// Everything above tests ConfigLoader::resolve_backtest_window. None of it
// touches the three runners, so reverting only apps/backtest/*.cpp -- putting
// the inline now()/mktime block back and losing the escape hatch entirely --
// left every one of those tests green. The helper existing is not the point;
// the runners USING it is.
//
// Source-scanned, like the F-5 tripwire, because what is being asserted is that
// a call site exists in a file this test does not link.
TEST(BacktestWindowM12, AllThreeBacktestRunnersGoThroughTheSharedHelper) {
    namespace fs = std::filesystem;
    const char* runners[] = {
        "apps/backtest/bt_portfolio.cpp",
        "apps/backtest/bt_portfolio_conservative.cpp",
        "apps/backtest/bt_equity_mean_reversion.cpp",
    };
    int checked = 0;
    for (const char* rel : runners) {
        const auto path = find_repo_file(rel);
        ASSERT_FALSE(path.empty()) << "could not locate " << rel;
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string src = ss.str();

        EXPECT_NE(src.find("resolve_backtest_window"), std::string::npos)
            << rel << " does not call ConfigLoader::resolve_backtest_window, so its window is "
                      "still computed inline and M-12 cannot freeze it";
        EXPECT_NE(src.find("M-12 FROZEN BACKTEST WINDOW"), std::string::npos)
            << rel << " does not announce a frozen window, so a frozen run would be "
                      "indistinguishable from a production one in the log";
        // The inline arithmetic the helper replaced must be gone, or a runner
        // could call the helper and then quietly overwrite its answer.
        EXPECT_EQ(src.find("start_tm.tm_year -= app_config.backtest.lookback_years"),
                  std::string::npos)
            << rel << " still computes the window inline as well as calling the helper";
        ++checked;
    }
    EXPECT_EQ(checked, 3);
}
