// tests/strategy/test_signals_publication.cpp
//
// Drift-G + B-3's F-1, decided together: WHO publishes a signal, and to which table.
//
// The E2 reconciliation checklist listed `backtest.signals` as a table to reconcile. It holds
// 18 rows, all from one strategy on one date in 2023, and no backtest path writes it: nothing
// under src/backtest/ or apps/backtest/ calls store_signals() or set_signals(). There is no
// writer to fix -- adding one is a feature, not a repair -- so the table comes off the
// checklist and this test is the tripwire that says so if a writer ever appears.
//
// The other half is the same question one level down. `MeanReversionStrategy::on_data()`
// computes a signal per symbol into a LOCAL map and drops it; it never calls
// `BaseStrategy::on_signal()`. That looks like an omission and is not: `on_signal()` writes
// `last_signals_`, whose only reader `get_last_signals()` has no callers anywhere. Adding the
// call would publish to nobody while making the strategy look like the publisher of record.
//
// The publisher of record for the equity book is the RUNNER: one `trading.signals` row per
// configured symbol per day, taken from `get_z_score()`. That is a real, reconcilable
// artefact -- 210 rows over the 21-day window A, ten symbols a day -- and it is what the
// checklist should name.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/strategy/base_strategy.hpp"

using namespace trade_ngin;

namespace {

std::filesystem::path find_repo_dir(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_all(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Every .cpp under a directory, concatenated.
std::string concat_sources(const std::string& relative_dir) {
    auto dir = find_repo_dir(relative_dir);
    if (dir.empty()) return {};
    std::string all;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".cpp") all += read_all(e.path());
    }
    return all;
}

// Lines that CALL something, ignoring the ones that only talk about it. A source-scan
// assertion that cannot tell a call from a comment fails on its own explanation -- this one
// did, on the very comment it exists to protect.
size_t count_non_comment_lines_containing(const std::string& src, const std::string& needle) {
    size_t hits = 0;
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 2, "//") == 0) continue;
        if (line.compare(first == std::string::npos ? 0 : first, 1, "*") == 0) continue;
        if (line.find(needle) != std::string::npos) ++hits;
    }
    return hits;
}

}  // namespace

TEST(SignalsPublication, NoBacktestPathPublishesSignalsSoTheTableIsNotAReconciliationSource) {
    const std::string backtest_lib = concat_sources("src/backtest");
    const std::string backtest_apps = concat_sources("apps/backtest");
    if (backtest_lib.empty() && backtest_apps.empty()) {
        GTEST_SKIP() << "backtest sources not found from the test working directory";
    }

    for (const auto& [where, src] : std::vector<std::pair<const char*, const std::string*>>{
             {"src/backtest", &backtest_lib}, {"apps/backtest", &backtest_apps}}) {
        EXPECT_EQ(count_non_comment_lines_containing(*src, "store_signals("), 0u)
            << where << " has acquired a signals writer. `backtest.signals` was dropped from "
                        "the E2 reconciliation checklist because nothing wrote it (18 rows, "
                        "one strategy, one date in 2023). If it is being written now, put it "
                        "back on the checklist and delete this expectation.";
        EXPECT_EQ(count_non_comment_lines_containing(*src, "set_signals("), 0u)
            << where << " has acquired a signals writer -- see above.";
    }
}

TEST(SignalsPublication, TheEquityRunnerIsThePublisherOfRecordAndUsesZScores) {
    auto path = find_repo_dir("apps/strategies/live_equity_mean_reversion.cpp");
    if (path.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
    const std::string src = read_all(path);

    // One row per configured symbol per day, from the z-score. This is the artefact the
    // checklist names for the equity book.
    EXPECT_NE(src.find("mr_strategy->get_z_score(symbol)"), std::string::npos);
    EXPECT_NE(src.find("results_manager->set_signals(signals_to_store)"), std::string::npos)
        << "the equity runner must remain the publisher of trading.signals; if it stops, the "
           "equity book has no signal record at all, because the strategy publishes none";
}

TEST(SignalsPublication, MeanReversionDeliberatelyDoesNotCallOnSignal) {
    auto path = find_repo_dir("src/strategy/mean_reversion.cpp");
    if (path.empty()) GTEST_SKIP() << "strategy source not found from the test working directory";
    const std::string src = read_all(path);

    // A CALL, not a mention: the comment recording this decision names on_signal() several
    // times, so a plain substring search finds its own explanation and fails. Scan lines and
    // ignore the ones that are comments.
    EXPECT_EQ(count_non_comment_lines_containing(src, "on_signal("), 0u)
        << "MeanReversionStrategy has started calling on_signal(). That publishes into "
           "BaseStrategy::last_signals_, whose only reader get_last_signals() has no callers, "
           "so it records nothing while making the strategy look like the publisher of "
           "record. If last_signals_ has acquired a real consumer, say so here and in "
           "LiveBacktestSignalConsistency, which asserts the map is empty on both sides.";

    // The decision is written down where the signal is computed, not only in a report.
    EXPECT_NE(src.find("Drift-G / F-1: this map is LOCAL and is deliberately never published"),
              std::string::npos);
}

TEST(SignalsPublication, TheThreeTrendStrategiesDoPublishAndAreUnaffected) {
    // The counterpart: on_signal() is not dead code, it is trend-following's. Nothing here
    // changes for them, and the asymmetry is intentional rather than an oversight.
    for (const auto* f : {"src/strategy/trend_following.cpp", "src/strategy/trend_following_fast.cpp",
                          "src/strategy/trend_following_slow.cpp"}) {
        auto path = find_repo_dir(f);
        if (path.empty()) GTEST_SKIP() << "strategy source not found: " << f;
        EXPECT_NE(read_all(path).find("on_signal(symbol,"), std::string::npos)
            << f << " no longer publishes signals";
    }
}
