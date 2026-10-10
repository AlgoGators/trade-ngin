// tests/live/test_mode_branch_guard.cpp
//
// T-DATED safeguard S2 (HD 2026-10-10). THIS TEST READS SOURCE, on purpose: it is a guard, not a
// behaviour test.
//
// A live runner has three ways of being run: with no date, with a date equal to the host's date
// (production), and with a past date (every test run). Stage 3 found protections that existed in
// only one of them. This guard fails when a line of the live apps, or of the library they call,
// reads "a date was given" (use_override_date), "the run date is the host's date"
// (run_date_is_host_date) or the wall clock, outside the lists below.
//
// TO ADD A SITE: add its line here with what it is for, and, when it DECIDES anything, name the
// test that EXECUTES both of its arms. A site that only stamps a row or a log line says so.
// Whoever adds a new mode branch has to write that test first.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path repo_root() {
    auto dir = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(dir / "apps/strategies/live_portfolio_conservative.cpp")) return dir;
        if (!dir.has_parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string trimmed(const std::string& line) {
    const auto a = line.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    const auto b = line.find_last_not_of(" \t\r");
    return line.substr(a, b - a + 1);
}

bool is_comment(const std::string& t) {
    return t.rfind("//", 0) == 0 || t.rfind("*", 0) == 0 || t.rfind("/*", 0) == 0;
}

// The code lines of a file (comment lines left out) that match the pattern, trimmed, in order.
std::vector<std::string> matching_lines(const std::filesystem::path& file, const std::regex& what) {
    std::vector<std::string> out;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trimmed(line);
        if (t.empty() || is_comment(t)) continue;
        if (std::regex_search(t, what)) out.push_back(t);
    }
    return out;
}

const std::regex kAppSites(
    R"(use_override_date|system_clock::now\(|run_date_is_host_date\(|std::time\(|localtime\()");
const std::regex kLibraryClock(
    R"(system_clock::now\(|steady_clock::now\(|std::time\(|[^_a-zA-Z]time\((nullptr|NULL|0)\)|[^_a-zA-Z]localtime\(|run_date_is_host_date\()");

struct Site {
    const char* line;  ///< the trimmed code line
    const char* what;  ///< what it is for; a DECISION names the test that executes both arms
};

// --- the futures twins (the same lines, in the same order, in both) ---------------------------
const std::vector<Site> kFuturesSites = {
    {"bool use_override_date = false;", "the flag: a date argument was given"},
    {"use_override_date = true;", "set by the argument parse"},
    {"if (!use_override_date) {",
     "DECISION: a run with no date mails by default. Not run in tests by rule (it mails); the "
     "report plan itself: EmailReportPlan.* (tests/live/test_email_body_file.cpp)"},
    {"if (send_email && use_override_date) {", "a console line"},
    {"auto now = use_override_date ? target_date : std::chrono::system_clock::now();",
     "THE RUN CLOCK: the date given, or the wall clock on a run with no date"},
    {"core::safe_localtime(&now_time_t, &now_tm_fields);",
     "the run date's local-time fields, copied (console date, email date, a Sunday log line)"},
    {"auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;",
     "the bar window's end: T-1 on a dated run"},
    {"INFO(\"DEBUG: Run type: \" + std::string(use_override_date ? \"HISTORICAL\" : \"LIVE\"));",
     "a log line"},
    {"use_override_date, \"Futures\");",
     "DECISION: rule_feed_freshness(dated_run). Both arms executed by FeedFreshnessRuling.* "
     "(tests/live/test_held_feed_ruling.cpp)"},
    {"held_holes, hole_tolerance_days, now, std::chrono::system_clock::now());",
     "DECISION: the held-hole refusal is for the host's own date (rule_held_feed_holes). Both "
     "arms executed by HeldFeedRuling.* (tests/live/test_held_feed_ruling.cpp) and, on the real "
     "binary, by the mode gate's production-mode chain (scripts/live_mode_gate.sh)"},
    {"if (!use_override_date) {", "the processing-time print: a console line"},
    {"std::chrono::system_clock::now() - now)", "the processing-time print"},
};

// --- the equity runner ------------------------------------------------------------------------
const std::vector<Site> kEquitySites = {
    {"bool use_override_date = false;", "the flag: a date argument was given"},
    {"use_override_date = true;", "set by the argument parse"},
    {"if (!use_override_date) {",
     "DECISION: a run with no date mails by default. Not run in tests by rule; the report plan: "
     "EmailReportPlan.*"},
    {"if (send_email && use_override_date) {", "a console line"},
    {"if (use_override_date) {", "the log directory of a dated run (logs/<date>/): a file name"},
    {"auto now = use_override_date ? target_date : std::chrono::system_clock::now();",
     "THE RUN CLOCK"},
    {"auto end_date = use_override_date ? (now - std::chrono::hours(24)) : now;",
     "the bar window's end: T-1 on a dated run"},
    {"DEBUG(\"Run type: \" + std::string(use_override_date ? \"HISTORICAL\" : \"LIVE\"));",
     "a log line"},
    {"use_override_date, \"Equity\");",
     "DECISION: rule_feed_freshness(dated_run). Both arms executed by FeedFreshnessRuling.*"},
    {"feed_holes.held, hole_tolerance_days, now, std::chrono::system_clock::now());",
     "DECISION: the held-hole refusal is for the host's own date. Both arms executed by "
     "HeldFeedRuling.* (the equity selection: "
     "TheEquityRunnersSelectionFromItsNewestBarsFollowsTheSameRule)"},
    {"if (!use_override_date) {", "the processing-time print: a console line"},
    {"std::chrono::system_clock::now() - now).count() << \"ms\" << std::endl;",
     "the processing-time print"},
};

// --- the library: how many wall-clock reads each file holds, and what they are -----------------
// A file not listed must hold none. A count that changes means a clock read was added or removed:
// look at it, and say here what it is.
const std::map<std::string, std::pair<int, const char*>> kLibraryClockReads = {
    {"include/trade_ngin/live/session_book_gate.hpp",
     {2, "DECISION: run_date_is_host_date and its one caller rule_held_feed_holes; the host "
         "clock is passed IN by the runner. Executed by HeldFeedRuling.* and "
         "SessionBookGate.TheRefusalIsForTheHostsOwnDateOnly"}},
    {"include/trade_ngin/core/timed_process.hpp",
     {4, "the chart process's time limit (a steady clock, a duration). Executed by TimedProcess.*"}},
    {"include/trade_ngin/core/time_utils.hpp", {1, "a formatting helper's default argument"}},
    {"include/trade_ngin/live/live_price_manager.hpp", {1, "a default argument no live runner uses"}},
    {"src/core/logger.cpp", {2, "log file names and line stamps"}},
    {"src/core/email_sender.cpp", {2, "the MIME boundary of a sent mail"}},
    {"src/core/state_manager.cpp", {2, "in-memory component stamps"}},
    {"src/core/config_loader.cpp", {1, "the backtest window; no live runner calls it"}},
    {"src/data/postgres_database.cpp",
     {3, "a component stamp, and the fallback when a stored last_update does not parse (WARN)"}},
    {"src/data/database_pooling.cpp", {2, "connection-pool wait timing"}},
    {"src/strategy/base_strategy.cpp", {2, "in-memory component stamps"}},
    {"src/strategy/trend_following.cpp",
     {1, "the initial last_update of a position, overwritten with the run date before a store"}},
    {"src/strategy/mean_reversion.cpp", {1, "as trend_following.cpp"}},
    {"src/strategy/regime_detector.cpp", {1, "not on a live runner's path"}},
    {"src/portfolio/portfolio_manager.cpp",
     {4, "a component stamp, and the fill-time fallback when no time is passed and no bar is "
         "loaded (not reached with bars loaded)"}},
    {"src/live/live_pnl_manager.cpp", {1, "a snapshot stamp nothing stores"}},
    {"src/execution/execution_engine.cpp", {8, "the execution engine; no live runner uses it"}},
    {"src/order/order_manager.cpp", {4, "the order manager; no live runner uses it"}},
    {"src/instruments/option.cpp", {3, "options; no live runner uses them"}},
    {"src/instruments/futures.cpp", {1, "is_tradeable(); no live caller"}},
    {"src/storage/backtest_results_manager.cpp", {3, "backtest only"}},
    {"src/backtest/backtest_coordinator.cpp", {1, "backtest only"}},
};

void expect_sites(const std::string& relative, const std::vector<Site>& allowed) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found from the test working directory";
    const auto found = matching_lines(root / relative, kAppSites);
    std::vector<std::string> want;
    for (const auto& s : allowed) want.emplace_back(s.line);
    EXPECT_EQ(found, want)
        << relative
        << ": a line that reads the date flag, the host-date test or the wall clock was added, "
           "removed or changed. Add it to the list in tests/live/test_mode_branch_guard.cpp with "
           "what it is for and the test that executes both of its arms.";
}

}  // namespace

TEST(ModeBranchGuard, TheConservativeFuturesRunnerReadsTheModeOnlyAtTheListedSites) {
    expect_sites("apps/strategies/live_portfolio_conservative.cpp", kFuturesSites);
}

TEST(ModeBranchGuard, TheBaseFuturesRunnerReadsTheModeOnlyAtTheListedSites) {
    expect_sites("apps/strategies/live_portfolio.cpp", kFuturesSites);
}

TEST(ModeBranchGuard, TheEquityRunnerReadsTheModeOnlyAtTheListedSites) {
    expect_sites("apps/strategies/live_equity_mean_reversion.cpp", kEquitySites);
}

TEST(ModeBranchGuard, NoLiveRunnerKeepsThePointerStdLocaltimeReturns) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    const std::regex pointer(R"(=\s*std::localtime\()");
    for (const char* app : {"apps/strategies/live_portfolio_conservative.cpp",
                            "apps/strategies/live_portfolio.cpp",
                            "apps/strategies/live_equity_mean_reversion.cpp"}) {
        EXPECT_TRUE(matching_lines(root / app, pointer).empty())
            << app << " keeps a pointer into the C library's static struct";
    }
}

TEST(ModeBranchGuard, TheLibraryReadsTheWallClockOnlyInTheListedFiles) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found";
    std::map<std::string, int> found;
    for (const char* top : {"src", "include"}) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / top)) {
            if (!entry.is_regular_file()) continue;
            const auto ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".hpp") continue;
            const int n = static_cast<int>(matching_lines(entry.path(), kLibraryClock).size());
            if (n > 0) found[std::filesystem::relative(entry.path(), root).generic_string()] = n;
        }
    }
    std::map<std::string, int> want;
    for (const auto& [file, entry] : kLibraryClockReads) want[file] = entry.first;
    EXPECT_EQ(found, want)
        << "a wall-clock read was added to or removed from the library. Say what it is in "
           "kLibraryClockReads (tests/live/test_mode_branch_guard.cpp); if it DECIDES anything on "
           "a live run, name the test that executes both of its arms.";
}
