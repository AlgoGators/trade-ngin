// C-5 §9-A1 -- the E2-F41 announcement must be asserted, not assumed.
//
// THE TEST DEFECT: 19d44c45 added the class-1 cross-check WARN to the runner and claimed
// test_corp_action_query_bounds_db.cpp as its pin. Executed revert (C-5 L-EVIDENCE Tier C):
// with apps/strategies/live_equity_mean_reversion.cpp reverted to 19d44c45^, all NINE tests
// in that suite still PASS:
//
//     [==========] 9 tests from CorpActionQueryBoundsDbTest ran. (79 s)
//     [  PASSED  ] 9 tests.
//
// The suite measures the DATABASE -- that the silent-row class exists and is bounded. That
// is a real and useful tripwire, but it is not a pin on the runner's announcement: it holds
// whether or not the runner says anything. B-5b's claimed red was a TEST-side revert (the
// old two-symbol allowlist against the widened scan), which proves the tripwire's reach over
// the universe, not the fix.
//
// The announcement itself was a WARN emitted inside a runner loop, which no unit test can
// observe. It is now a returned value -- class1_rows_the_bars_do_not_carry() -- so what the
// run announces is a function of its inputs and can be asserted.

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/live/corp_action_feed_status.hpp"

using namespace trade_ngin;

namespace {
using Key = std::pair<std::string, std::string>;
}

TEST(Class1SilentRowAnnouncement, TheProvenLenInstanceIsAnnounced) {
    // LEN 2025-02-07: `spinoff` MRP 0.5 and `spinoffdividend` 11.495 in corporate_action,
    // while the LEN bar for that date carries split_factor 1 and div_cash 0 -- so the
    // per-bar feed produced NO row for it and the applier is a silent no-op.
    const std::vector<Key> terms = {{"LEN", "2025-02-07"}};
    const std::set<Key> per_bar = {{"AAPL", "2025-02-07"}, {"LEN", "2024-11-15"}};

    const auto silent = class1_rows_the_bars_do_not_carry(terms, per_bar);

    ASSERT_EQ(silent.size(), 1u) << "a class-1 row with no bar behind it must be announced";
    EXPECT_EQ(silent[0].first, "LEN");
    EXPECT_EQ(silent[0].second, "2025-02-07");
}

TEST(Class1SilentRowAnnouncement, ARowTheBarsDoCarryIsNotAnnounced) {
    // The applier will act on it. Announcing it would train the operator to ignore the line.
    const std::vector<Key> terms = {{"LEN", "2025-02-07"}};
    const std::set<Key> per_bar = {{"LEN", "2025-02-07"}};
    EXPECT_TRUE(class1_rows_the_bars_do_not_carry(terms, per_bar).empty());
}

TEST(Class1SilentRowAnnouncement, TheKeyIsTickerAndDateTogether) {
    // Same ticker, different date, and same date, different ticker: neither matches.
    const std::vector<Key> terms = {{"LEN", "2025-02-07"}};
    EXPECT_EQ(class1_rows_the_bars_do_not_carry(terms, {{"LEN", "2025-02-06"}}).size(), 1u);
    EXPECT_EQ(class1_rows_the_bars_do_not_carry(terms, {{"LEM", "2025-02-07"}}).size(), 1u);
}

TEST(Class1SilentRowAnnouncement, EverySilentRowIsAnnouncedNotJustTheFirst) {
    // 99 rows universe-wide since 2020. A run that announced only the first would hide the
    // rest of the class behind one line.
    const std::vector<Key> terms = {
        {"LEN", "2025-02-07"}, {"ABT", "2004-05-03"}, {"MMM", "2024-04-01"}};
    const std::set<Key> per_bar = {{"MMM", "2024-04-01"}};

    const auto silent = class1_rows_the_bars_do_not_carry(terms, per_bar);
    ASSERT_EQ(silent.size(), 2u);
    EXPECT_EQ(silent[0].first, "LEN") << "feed order is preserved so the log is stable";
    EXPECT_EQ(silent[1].first, "ABT");
}

TEST(Class1SilentRowAnnouncement, NoTermsRowsMeansNothingToAnnounce) {
    EXPECT_TRUE(class1_rows_the_bars_do_not_carry({}, {{"LEN", "2025-02-07"}}).empty());
}

TEST(Class1SilentRowAnnouncement, NoBarsAtAllAnnouncesEveryTermsRow) {
    // The shape of a run whose per-bar fetch returned nothing: every class-1 row in the
    // window is unapplied, and all of them must be said out loud.
    const std::vector<Key> terms = {{"LEN", "2025-02-07"}, {"ABT", "2004-05-03"}};
    EXPECT_EQ(class1_rows_the_bars_do_not_carry(terms, {}).size(), 2u);
}
