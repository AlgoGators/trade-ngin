// tests/live/test_day_t_write_ordering.cpp
//
// E2-F44 / BA-18 -- a fatal in-run identity must not leave a half-written day.
//
// The equity runner asserts protocol L5 twice inside the day, and both assertions are
// FATAL: realized (`Σ positions.daily_realized_pnl == live_results.daily_realized_pnl`) and
// unrealized (`Σ positions.daily_unrealized_pnl == live_results.total_unrealized_pnl`).
// Both used to run AFTER the unconditional `DELETE FROM trading.positions ... DATE(last_update)
// = today` that R-3 added, so a violated identity exited 1 having already removed today's
// rows -- the day ended up empty as well as unwritten, and the previous run's rows, which a
// re-run would simply have overwritten, were gone.
//
// The fix is an ORDERING one: the clear-then-write pair belongs together, immediately before
// save_all_results, with every fatal check ahead of it. The runner is a `main()` and cannot
// be linked into this binary, so what is tested is that ordering in the source -- the same
// approach tests/core/test_equity_portfolio_namespace.cpp takes for the same reason. It
// fails the moment the DELETE drifts back above either assertion.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_runner() {
    auto path = find_repo_file("apps/strategies/live_equity_mean_reversion.cpp");
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

TEST(DayTWriteOrdering, TheClearRunsAfterBothFatalL5IdentitiesAndNotBefore) {
    const std::string src = read_runner();
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    const auto clear = src.find("DELETE FROM trading.positions WHERE strategy_id = ");
    const auto realized = src.find("L5 realized identity VIOLATED");
    const auto unrealized = src.find("L5 unrealized identity VIOLATED");
    const auto save = src.find("save_all_results(\"LIVE_EQUITY_MEAN_REVERSION\"");

    ASSERT_NE(clear, std::string::npos) << "the R-3 clear-today DELETE is gone entirely";
    ASSERT_NE(realized, std::string::npos);
    ASSERT_NE(unrealized, std::string::npos);
    ASSERT_NE(save, std::string::npos);

    EXPECT_GT(clear, realized)
        << "the clear-today DELETE runs BEFORE the fatal realized L5 assertion again: a "
           "violated identity would exit 1 with today's position rows already deleted and "
           "nothing written in their place (E2-F44)";
    EXPECT_GT(clear, unrealized)
        << "the clear-today DELETE runs BEFORE the fatal unrealized L5 assertion again "
           "(E2-F44)";
    EXPECT_LT(clear, save)
        << "the clear-today DELETE must still precede save_all_results -- it is the only "
           "statement that removes today's position rows, because delete_stale_data() covers "
           "live_results, equity_curve and executions but NOT positions (E2-F19 R-3)";
}

TEST(DayTWriteOrdering, BothFatalExitsSayWhatTheyLeftBehindAndHowToRecover) {
    const std::string src = read_runner();
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    // Two STATE AT THIS EXIT blocks, one before each fatal identity exit. "The run failed"
    // and "the database is unchanged" are different statements, and an operator reading the
    // log has to be told which one applies before deciding whether to re-run or to reset.
    size_t occurrences = 0;
    for (size_t at = src.find("STATE AT THIS EXIT"); at != std::string::npos;
         at = src.find("STATE AT THIS EXIT", at + 1)) {
        ++occurrences;
    }
    EXPECT_EQ(occurrences, 2u)
        << "each fatal L5 exit must name the tables already written and the recovery";

    // The recovery rule itself, which E2_RUN_PROTOCOL.md replay rule 10 also carries.
    EXPECT_NE(src.find("re-run THIS SAME DATE"), std::string::npos);
    EXPECT_NE(src.find("reset the book WINDOWED"), std::string::npos);
    EXPECT_NE(src.find("corp_action_applied together (replay rule 7)"), std::string::npos);
}
