// tests/live/test_margin_columns_runners.cpp
//
// T-8D R21 and section 3.14 -- the margin and leverage cells of trading.live_results, as the
// three live runners write them.
//
// The Day T-1 finalize settles current_portfolio_value and rewrites cash_available on it. It
// used to leave portfolio_leverage and equity_to_margin_ratio as the day's own run wrote them
// ("only update if NULL or 0") and never touched net_leverage or margin_cushion, so a settled
// row stood on two values of the account. And a day with no posted margin stored a cushion of
// -1 and a ratio of 0, which the "below 20%" warning then fired on.
//
// The runners are `main()`s and cannot be linked into this binary, so what is tested is the
// structure in the source, the approach tests/live/test_day_t_write_ordering.cpp takes for the
// same reason. The arithmetic of the helper they call is in tests/live/test_margin_columns.cpp.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

std::string read_source(const std::string& relative) {
    auto path = find_repo_file(relative);
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

const char* const kConservative = "apps/strategies/live_portfolio_conservative.cpp";
const char* const kBase = "apps/strategies/live_portfolio.cpp";
const char* const kEquity = "apps/strategies/live_equity_mean_reversion.cpp";
const std::vector<std::string> kRunners = {kConservative, kBase, kEquity};

const char* const kStep4 = "STEP 4: UPDATE Day T-1 live_results AND equity_curve";
const char* const kStep4b = "STEP 4b: THE STATISTICS THROUGH THE LAST SETTLED ROW (Day T-1)";

// STEP 4 of a runner: the Day T-1 finalize, up to the statistics block.
std::string finalize_step(const std::string& src) {
    const auto begin = src.find(kStep4);
    const auto end = src.find(kStep4b);
    if (begin == std::string::npos || end == std::string::npos || end < begin) return {};
    return src.substr(begin, end - begin);
}

}  // namespace

TEST(MarginColumnsRunners, TheFinalizeNoLongerPreservesTheCellsOfTheDaysOwnRun) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const std::string finalize = finalize_step(src);
        ASSERT_FALSE(finalize.empty()) << runner;

        EXPECT_EQ(count_of(finalize, "ELSE portfolio_leverage END"), 0u)
            << runner << ": the finalize keeps the leverage of the day's own run, on a "
                         "portfolio value the same statement replaces (R21)";
        EXPECT_EQ(count_of(finalize, "ELSE equity_to_margin_ratio END"), 0u)
            << runner << ": the finalize keeps the ratio of the day's own run (R21), and "
                         "writes 0 over a NULL";
        // The preserve read: the loader that maps a NULL ratio to 0.
        EXPECT_EQ(count_of(finalize, "load_margin_metrics("), 0u) << runner;
    }
}

TEST(MarginColumnsRunners, TheFinalizeRecomputesTheFourCellsOnTheFinalisedValue) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        const std::string finalize = finalize_step(src);
        ASSERT_FALSE(finalize.empty()) << runner;

        EXPECT_EQ(count_of(src, "finalize_margin_columns_sql("), 1u) << runner;
        EXPECT_EQ(count_of(finalize, "finalize_margin_columns_sql("), 1u)
            << runner << ": the four cells are not rewritten by the Day T-1 UPDATE";
        // The fragment sits in the statement that settles the value, before cash_available.
        const auto update = finalize.find("\"UPDATE trading.live_results SET \"");
        const auto fragment = finalize.find("finalize_margin_columns_sql(");
        const auto cash = finalize.find("\"cash_available = ");
        ASSERT_NE(update, std::string::npos) << runner;
        ASSERT_NE(cash, std::string::npos) << runner;
        EXPECT_LT(update, fragment) << runner;
        EXPECT_LT(fragment, cash) << runner;
    }
}

TEST(MarginColumnsRunners, NoSentinelIsStoredForACushionOrARatioWithoutMargin) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        EXPECT_EQ(count_of(src, "margin_cushion = -1.0"), 0u)
            << runner << ": a day with no requirement stores a cushion of -1 (P-6)";
        // The two cells reach the INSERT only through the helper that leaves an absent value out.
        EXPECT_EQ(count_of(src, "{\"margin_cushion\", margin_cushion}"), 0u) << runner;
        EXPECT_EQ(count_of(src, "{\"equity_to_margin_ratio\", equity_to_margin_ratio}"), 0u)
            << runner;
        EXPECT_EQ(count_of(src, "set_margin_cells(double_metrics, equity_to_margin_ratio, "
                                "margin_cushion);"),
                  1u)
            << runner;
    }
}

TEST(MarginColumnsRunners, TheCushionWarningNeedsACushion) {
    for (const auto& runner : kRunners) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

        EXPECT_EQ(count_of(src, "if (margin_cushion < 0.20)"), 0u)
            << runner << ": \"Margin cushion below 20%\" fires on a day with no cushion";
        EXPECT_EQ(count_of(src, "if (margin_cushion && *margin_cushion < 0.20)"), 1u) << runner;
    }
}

TEST(MarginColumnsRunners, TheFuturesRunnersFinalizeTheFourCellsIdentically) {
    const std::string a = finalize_step(read_source(kConservative));
    const std::string b = finalize_step(read_source(kBase));
    if (a.empty() || b.empty()) {
        GTEST_SKIP() << "runner source not found from the test working directory";
    }
    const char* const kFrom = "// T-8D R21: the four cells of the Day T-1 row";
    const char* const kTo = "INFO(\"Executing UPDATE query for Day T-1 live_results...\");";
    const auto a0 = a.find(kFrom), a1 = a.find(kTo);
    const auto b0 = b.find(kFrom), b1 = b.find(kTo);
    ASSERT_NE(a0, std::string::npos);
    ASSERT_NE(a1, std::string::npos);
    ASSERT_NE(b0, std::string::npos);
    ASSERT_NE(b1, std::string::npos);
    EXPECT_EQ(a.substr(a0, a1 - a0), b.substr(b0, b1 - b0));
}

// T-8D R21 (HD 2026-09-10): the equity runner's ratio is the current portfolio value over the
// posted margin, at the level of the book. It was gross notional over margin, 1.0 by
// construction in a cash account, with two warnings written for that figure.
TEST(MarginColumnsRunners, TheEquityRatioIsThePortfolioValueOverThePostedMargin) {
    const std::string src = read_source(kEquity);
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    EXPECT_EQ(count_of(src, "equity_to_margin_ratio_of(gross_notional"), 0u);
    EXPECT_EQ(count_of(src, "gross_notional / total_posted_margin"), 0u);
    EXPECT_EQ(count_of(src, "equity_to_margin_ratio_of(current_portfolio_value, "
                            "total_posted_margin)"),
              1u);
    // The same call the futures runners make.
    for (const char* runner : {kConservative, kBase}) {
        const std::string fut = read_source(runner);
        EXPECT_EQ(count_of(fut, "equity_to_margin_ratio_of(current_portfolio_value, "
                                "total_posted_margin)"),
                  1u)
            << runner;
    }
}

TEST(MarginColumnsRunners, TheEquityRunnersTwoRatioWarningsAreRetired) {
    const std::string src = read_source(kEquity);
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    EXPECT_EQ(count_of(src, "is <= 1.0; verify margins"), 0u)
        << "the warning written for gross notional over margin still fires";
    EXPECT_EQ(count_of(src, "Equity-to-Margin Ratio above 4x"), 0u)
        << "on value over margin a ratio above 4 is the normal state of the book";
    // The check on the account stays.
    EXPECT_EQ(count_of(src, "Posted margin exceeds current portfolio value"), 1u);
}

// T-8D D4 (a): a cash account has no maintenance requirement, so the finalize recomputes the
// cushion of an equity row against none.
TEST(MarginColumnsRunners, TheEquityFinalizeRecomputesTheCushionAgainstNoMaintenance) {
    const std::string finalize = finalize_step(read_source(kEquity));
    if (finalize.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    EXPECT_EQ(count_of(finalize, "yesterday_maintenance_sql = \"0.0\";"), 1u);
    EXPECT_EQ(count_of(finalize, "yesterday_maintenance_sql = \"COALESCE(margin_posted"), 0u);
}

// T-8D R22: on the equity book gross_leverage and net_leverage are the row's notional over its
// portfolio value, at the day's write and in the finalize. They were the risk report's figures
// (positions at cost basis over the configured capital), which the finalize never revisited.
TEST(MarginColumnsRunners, TheEquityLeverageColumnsAreNotionalOverThePortfolioValue) {
    const std::string src = read_source(kEquity);
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    EXPECT_EQ(count_of(src, "gross_leverage = r.gross_leverage;"), 0u)
        << "gross_leverage is still the risk report's figure";
    EXPECT_EQ(count_of(src, "net_leverage = r.net_leverage;"), 0u)
        << "net_leverage is still the risk report's figure";
    EXPECT_EQ(count_of(src, "gross_leverage = portfolio_leverage;"), 1u);
    EXPECT_EQ(count_of(src, "(net_notional / current_portfolio_value) : 0.0;"), 2u)
        << "the stored cell and the email line";
    // Both columns are still written: AlgoLens reads them.
    EXPECT_EQ(count_of(src, "{\"gross_leverage\", gross_leverage},"), 1u);
    EXPECT_EQ(count_of(src, "{\"net_leverage\", net_leverage},"), 1u);

    const std::string finalize = finalize_step(src);
    ASSERT_FALSE(finalize.empty());
    EXPECT_EQ(count_of(finalize, "finalize_gross_leverage_sql(yesterday_finalised_value_sql)"), 1u)
        << "the finalize leaves gross_leverage on the value of the day's own run";
}

// The futures rows keep gross_leverage NULL: neither write names the column.
TEST(MarginColumnsRunners, TheFuturesRunnersDoNotWriteGrossLeverage) {
    for (const char* runner : {kConservative, kBase}) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, "{\"gross_leverage\""), 0u) << runner;
        EXPECT_EQ(count_of(src, "finalize_gross_leverage_sql("), 0u) << runner;
        EXPECT_EQ(count_of(src, "\"gross_leverage = "), 0u) << runner;
    }
}
