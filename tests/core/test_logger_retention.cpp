// tests/core/test_logger_retention.cpp
//
// drift-F -- log retention defeated the reconciliation it exists to support.
//
// Two separate mechanisms, both fixed here:
//
//  1. Rotation deleted the oldest regular file of ANY name in `log_directory`. Four runners
//     share `logs/`, so a futures session's retention budget evicted the equity session's
//     evidence and vice versa. The budget was global where the retention question is per
//     runner.
//  2. Ten files per directory over a 126-day replay leaves eight. The drift audit found
//     exactly that: the dividend-applying runs and the holiday case -- the days E2's
//     log-vs-DB reconciliation most needs to read afterwards -- were gone before anyone
//     looked. A replay now writes into `logs/<YYYY-MM-DD>/`, so each date has its own budget.
//
// Both changes are default-preserving: an empty `log_subdirectory` reproduces the old path,
// and a directory containing one prefix behaves exactly as it did.
//
// B-5d adds the third mechanism, which B-5b's fix left open: the prefixes NEST, so scoping
// retention to "begins with the prefix" still had `live_trend` counting and deleting
// `live_trend_conservative`'s files. Ownership is now the whole filename shape.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "trade_ngin/core/logger.hpp"

using namespace trade_ngin;

namespace {

// A scratch directory that cleans itself up.
class TempLogDir {
public:
    TempLogDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("tn_logret_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path_);
    }
    ~TempLogDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void touch(const std::filesystem::path& p) {
    std::ofstream out(p);
    out << "x\n";
}

std::vector<std::string> names_in(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    if (!std::filesystem::exists(dir)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (std::filesystem::is_regular_file(e.path())) out.push_back(e.path().filename().string());
    }
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

LoggerConfig file_config(const std::filesystem::path& dir, const std::string& prefix,
                         const std::string& subdir = {}) {
    LoggerConfig c;
    c.destination = LogDestination::FILE;
    c.log_directory = dir.string();
    c.log_subdirectory = subdir;
    c.filename_prefix = prefix;
    c.max_files = 3;
    return c;
}

}  // namespace

TEST(LoggerRetention, RotationNeverDeletesAnotherRunnersLogs) {
    TempLogDir dir;
    // Four foreign files, well over this logger's budget of 3. Before the fix the oldest of
    // these was deleted on every initialize(), which is how a futures run evicted an equity
    // replay's log out of the shared `logs/` directory.
    for (const auto* name : {"live_portfolio_conservative_20260101_000000_part1.log",
                             "live_portfolio_20260101_000001_part1.log",
                             "bt_equity_mr_20260101_000002_part1.log",
                             "some_operators_notes.txt"}) {
        touch(dir.path() / name);
    }

    Logger::instance().initialize(file_config(dir.path(), "live_equity_mr"));
    INFO("one line so the file exists");

    const auto after = names_in(dir.path());
    EXPECT_TRUE(contains(after, "live_portfolio_conservative_20260101_000000_part1.log"));
    EXPECT_TRUE(contains(after, "live_portfolio_20260101_000001_part1.log"));
    EXPECT_TRUE(contains(after, "bt_equity_mr_20260101_000002_part1.log"));
    EXPECT_TRUE(contains(after, "some_operators_notes.txt"))
        << "retention deleted a file this logger does not own";
}

// The prefixes NEST, and "begins with" is not ownership.
//
// `live_trend` is a prefix of `live_trend_conservative`; `bt_portfolio` of
// `bt_portfolio_conservative`. B-5b scoped retention to files whose name STARTS WITH the
// logger's prefix, which fixed the equity-versus-futures eviction and left the two futures
// books sharing one budget: every `live_portfolio` run counted -- and deleted -- the
// conservative runner's logs. The conservative runner is the production futures book, so the
// half of the pair whose evidence was still being destroyed was the one that matters most.
TEST(LoggerRetention, ABaseRunnerDoesNotEvictItsConservativeNamesake) {
    TempLogDir dir;
    // Four conservative logs, well over the base runner's budget of 3, and one of the base
    // runner's own so the directory is not trivially foreign.
    const std::vector<std::string> conservative = {
        "live_trend_conservative_20260101_000000_part1.log",
        "live_trend_conservative_20260101_000001_part1.log",
        "live_trend_conservative_20260101_000002_part1.log",
        "live_trend_conservative_20260101_000003_part1.log"};
    for (const auto& n : conservative) touch(dir.path() / n);
    touch(dir.path() / "live_trend_20260101_000004_part1.log");

    Logger::instance().initialize(file_config(dir.path(), "live_trend"));
    INFO("the base futures runner starts a session");

    const auto after = names_in(dir.path());
    for (const auto& n : conservative) {
        EXPECT_TRUE(contains(after, n))
            << "live_trend retention deleted " << n
            << " -- `live_trend` is a PREFIX of `live_trend_conservative`, so a "
               "starts-with test hands the two futures books one shared budget and lets the "
               "base runner destroy the production book's evidence";
    }
}

// The same pair the other way round, and the backtest pair too. A conservative session must
// not evict the base runner either -- the nesting only runs one way, but the ownership test
// has to be exact in both directions or the fix is half a fix.
TEST(LoggerRetention, NeitherHalfOfANestedPrefixPairTouchesTheOther) {
    TempLogDir dir;
    const std::vector<std::string> base = {"bt_portfolio_20260101_000000_part1.log",
                                           "bt_portfolio_20260101_000001_part1.log",
                                           "bt_portfolio_20260101_000002_part1.log",
                                           "bt_portfolio_20260101_000003_part1.log"};
    for (const auto& n : base) touch(dir.path() / n);

    Logger::instance().initialize(file_config(dir.path(), "bt_portfolio_conservative"));
    INFO("the conservative backtest starts a session");

    const auto after = names_in(dir.path());
    for (const auto& n : base) {
        EXPECT_TRUE(contains(after, n)) << "bt_portfolio_conservative deleted " << n;
    }
    // And its own file was created despite four foreign ones already sitting there.
    bool wrote_its_own = false;
    for (const auto& n : after) {
        if (n.rfind("bt_portfolio_conservative_", 0) == 0) wrote_its_own = true;
    }
    EXPECT_TRUE(wrote_its_own);
}

// Ownership is the whole filename, not a separator. Retention only ever removes files this
// logger actually wrote, so operator notes and anything under an older naming scheme survive.
TEST(LoggerRetention, OnlyFilesMatchingThisLoggersOwnFilenameShapeAreCounted) {
    TempLogDir dir;
    const std::vector<std::string> not_ours = {
        "live_trend_notes.txt",                          // an operator's file
        "live_trend_20260101.log",                       // no time, no part
        "live_trend_20260101_000000.log",                // no part
        "live_trend_2026011_000000_part1.log",           // seven-digit date
        "live_trend_20260101_000000_part.log",           // no part number
        "live_trend_20260101_000000_part1.log.bak",      // trailing junk
        "live_trend_conservative_20260101_000000_part1.log"};
    for (const auto& n : not_ours) touch(dir.path() / n);

    Logger::instance().initialize(file_config(dir.path(), "live_trend"));
    INFO("a session that must delete none of them");

    const auto after = names_in(dir.path());
    for (const auto& n : not_ours) {
        EXPECT_TRUE(contains(after, n)) << "retention deleted a file it did not write: " << n;
    }
    // Seven foreign files plus this session's one.
    EXPECT_EQ(after.size(), not_ours.size() + 1);
}

TEST(LoggerRetention, ItsOwnFilesAreStillCappedAtMaxFiles) {
    TempLogDir dir;
    // Five of this logger's own files and a budget of 3: two must go, oldest first, and the
    // rest stay. This is the behaviour that must NOT change.
    const std::vector<std::string> mine = {"live_equity_mr_20260101_000000_part1.log",
                                           "live_equity_mr_20260101_000001_part1.log",
                                           "live_equity_mr_20260101_000002_part1.log",
                                           "live_equity_mr_20260101_000003_part1.log",
                                           "live_equity_mr_20260101_000004_part1.log"};
    for (const auto& n : mine) {
        touch(dir.path() / n);
        // Distinct mtimes, oldest first, so "oldest" is well defined.
        std::filesystem::last_write_time(
            dir.path() / n,
            std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * (5 - (&n - &mine[0]))));
    }

    Logger::instance().initialize(file_config(dir.path(), "live_equity_mr"));
    INFO("one line so the file exists");

    const auto after = names_in(dir.path());
    // 3 is the budget: initialize() prunes down to max_files - 1 and then opens one.
    EXPECT_LE(after.size(), 3u) << "retention stopped capping this logger's own files";
    EXPECT_FALSE(contains(after, mine[0])) << "the oldest of this logger's files should go";
}

TEST(LoggerRetention, AReplayGetsItsOwnDatedDirectorySoNoDateEvictsAnother) {
    TempLogDir dir;

    // Two replayed dates, each with a budget of 3 and each writing more than one session.
    for (int session = 0; session < 4; ++session) {
        Logger::instance().initialize(file_config(dir.path(), "live_equity_mr", "2026-04-17"));
        INFO("17th, session " + std::to_string(session));
    }
    for (int session = 0; session < 4; ++session) {
        Logger::instance().initialize(file_config(dir.path(), "live_equity_mr", "2026-04-20"));
        INFO("20th, session " + std::to_string(session));
    }

    const auto d17 = dir.path() / "2026-04-17";
    const auto d20 = dir.path() / "2026-04-20";
    ASSERT_TRUE(std::filesystem::exists(d17));
    ASSERT_TRUE(std::filesystem::exists(d20));

    // The 17th survived the 20th entirely: eight sessions in one flat directory with a budget
    // of 3 would have left the 17th with nothing, which is precisely how a 126-day chain
    // ended with eight logs.
    EXPECT_GT(names_in(d17).size(), 0u)
        << "replaying a later date evicted an earlier date's logs";
    EXPECT_GT(names_in(d20).size(), 0u);
    EXPECT_LE(names_in(d17).size(), 3u) << "each date still respects max_files";
    EXPECT_LE(names_in(d20).size(), 3u);

    // And nothing landed in the flat directory.
    EXPECT_EQ(names_in(dir.path()).size(), 0u);
}

TEST(LoggerRetention, AnEmptySubdirectoryIsTheOldFlatPathExactly) {
    TempLogDir dir;
    Logger::instance().initialize(file_config(dir.path(), "live_equity_mr", /*subdir=*/""));
    INFO("flat");

    const auto flat = names_in(dir.path());
    ASSERT_EQ(flat.size(), 1u);
    EXPECT_EQ(flat[0].rfind("live_equity_mr_", 0), 0u);
    // No stray subdirectory was created.
    for (const auto& e : std::filesystem::directory_iterator(dir.path())) {
        EXPECT_FALSE(std::filesystem::is_directory(e.path()));
    }
}
