// tests/live/corp_actions/test_spinoff_bar_warnings.cpp
//
// Two things the spinoff pre-pass says about a bar, and the one it used to say wrongly.
//
// E2-F51 -- the SILENT half of the E2-F48 rule. A `split_factor < 1` on a spinoff ex-date is
// separated out and applied as a real reverse split; a `split_factor > 1` is not — it is folded
// into the distribution's own factor and the parent's share count does not move. That is right
// on all five such bars in this database, verified against `adjusted_close` on both sides of
// the bar, but it is a JUDGEMENT about what the vendor meant, not a fact about the column, and
// a genuine forward split coincident with a spinoff would fall outside it with no symptom
// except a share count that failed to grow. It has to be said out loud.
//
// The spurious REFUSED WARN -- `close_by_symbol_date` is fetched for symbols that have an
// event in the window AND are in the book (`event_symbols` filters on `previous_positions`).
// For a configured-but-not-held name every close is therefore 0.0, `dividend_factor()` falls
// back to exactly 1, and a dividend-encoded spinoff on that name looks like a bar with no
// distribution factor left. The pre-pass announced "SPINOFF REFUSED ... nothing to allocate"
// about a position the book does not hold, with numbers derived from a close that was never
// fetched. The row loop already skips a non-held ticker outright, so nothing could ever have
// been routed for one; the fix is to gate the announcements on the book actually holding it.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

namespace {

SpinoffBarColumns bar(double split, double div, double close) {
    SpinoffBarColumns c;
    if (split != 1.0) {
        c.has_split = true;
        c.split_factor = split;
    }
    if (div != 0.0) {
        c.has_dividend = true;
        c.dividend_cash = div;
    }
    c.close_at_ex_date = close;
    return c;
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

std::string read_runner() {
    auto path = find_repo_file("apps/strategies/live_equity_mean_reversion.cpp");
    if (path.empty()) return {};
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

size_t count_of(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

}  // namespace

TEST(SpinoffBarWarnings, EveryForwardSplitFoldedIntoTheDistributionIsFlagged) {
    // The five real bars where the vendor encodes part of the distribution in split_factor.
    // None of them changed a holder's share count; all five must announce that they were read
    // that way.
    struct Case { const char* sym; double split; double div; double close; };
    for (const auto& c : std::vector<Case>{{"ABT", 1.0688328345, 2.9154459753, 42.32},
                                           {"BX", 1.019, 0.5849224898, 31.43},
                                           {"K", 1.065, 3.67, 52.50},
                                           {"MET", 1.122, 5.87, 48.53},
                                           {"RTX", 1.589, 13.28, 49.93},
                                           {"FTV", 1.327, 0.0, 71.60}}) {
        const auto b = bar(c.split, c.div, c.close);
        EXPECT_TRUE(b.folds_a_forward_split()) << c.sym;
        EXPECT_FALSE(b.has_reverse_split()) << c.sym;
        // What the flag asserts: the whole step is the distribution's, share count unmoved.
        EXPECT_DOUBLE_EQ(b.spinoff_factor(), b.total_factor()) << c.sym;
    }
}

TEST(SpinoffBarWarnings, AReverseSplitOrNoSplitAtAllIsNotAForwardSplit) {
    // HLT, LDOS, DD -- the reverse-split bars, announced by their own WARN, not this one.
    for (const auto& b : {bar(0.3333333333, 24.48, 58.00), bar(0.25, 21.3548786447, 45.52),
                          bar(0.33333333, 0.0, 76.10)}) {
        EXPECT_FALSE(b.folds_a_forward_split());
        EXPECT_TRUE(b.has_reverse_split());
    }
    // MMM 2024-04-01: dividend-encoded, no split column at all.
    const auto mmm = bar(1.0, 17.3875, 94.02);
    EXPECT_FALSE(mmm.folds_a_forward_split());
    EXPECT_FALSE(mmm.has_reverse_split());
    // A split_factor of exactly 1 is the no-event value and is never a split either way.
    SpinoffBarColumns unit;
    unit.has_split = true;
    unit.split_factor = 1.0;
    EXPECT_FALSE(unit.folds_a_forward_split());
    EXPECT_FALSE(unit.has_reverse_split());
}

TEST(SpinoffBarWarnings, AMissingCloseMakesADividendEncodedBarLookRefusable) {
    // This is the mechanism behind the false alarm, pinned so the reason the gate exists
    // cannot be forgotten. MMM's real bar, with and without the close that is only fetched
    // for HELD symbols.
    const auto held = bar(1.0, 17.3875, 94.02);
    EXPECT_TRUE(held.routes_a_spinoff());
    EXPECT_NEAR(held.spinoff_factor(), 1.0 + 17.3875 / 94.02, 1e-12);

    const auto not_held = bar(1.0, 17.3875, 0.0);   // close never fetched
    EXPECT_DOUBLE_EQ(not_held.dividend_factor(), 1.0)
        << "an unusable close contributes the identity -- correct, because dividing a basis by "
           "0 or by a negative would be far worse than doing nothing";
    EXPECT_FALSE(not_held.routes_a_spinoff())
        << "so the bar reports 'no distribution factor left' purely because nobody fetched the "
           "close; announcing that as a REFUSAL is a false alarm, and the guard against it is "
           "that the runner only announces bars whose parent is actually held";
}

TEST(SpinoffBarWarnings, TheRunnerAnnouncesOnlyBarsWhoseParentIsHeld) {
    // The gate itself. The pre-pass is inside main() and cannot be linked, so it is asserted in
    // the source -- the approach tests/live/test_day_t_write_ordering.cpp takes for the same
    // reason. Both announcement loops over spinoff_bar_columns must skip a non-held parent.
    const std::string src = read_runner();
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";

    const size_t loops = count_of(src, "for (const auto& [key, col] : spinoff_bar_columns)");
    const size_t gates = count_of(src, "if (!book_holds(key.first)) continue;");
    ASSERT_GT(loops, 0u) << "the spinoff pre-pass announcement loops are gone entirely";
    EXPECT_EQ(gates, loops)
        << "every loop that announces a spinoff bar must skip a parent the book does not hold: "
           "close_by_symbol_date is fetched for held symbols only, so every factor in those "
           "lines is computed from a close of 0.0 for a non-held name, and the REFUSED line in "
           "particular reports a refusal that never had anything to refuse";

    // BA-27: and "holds" means a NON-ZERO quantity. `previous_positions` can carry a symbol
    // at quantity 0; a closed position has nothing to distribute, and apply_spinoffs refuses
    // it outright, so the announcement was the only thing that spoke about it.
    EXPECT_NE(src.find("auto book_holds = [&previous_positions](const std::string& symbol)"),
              std::string::npos);
    const auto holds_at = src.find("auto book_holds =");
    ASSERT_NE(holds_at, std::string::npos);
    const std::string holds_body = src.substr(holds_at, 400);
    EXPECT_NE(holds_body.find("quantity.as_double()) > 1e-9"), std::string::npos)
        << "book_holds must require a non-zero quantity, not mere presence in the map (BA-27)";

    // And the forward-split announcement exists and is driven by the predicate, not by an
    // inline comparison that can drift from it.
    EXPECT_NE(src.find("col.folds_a_forward_split()"), std::string::npos);
    EXPECT_NE(src.find("split_factor ABOVE 1"), std::string::npos);
    EXPECT_NE(src.find("(E2-F51)"), std::string::npos);
}
