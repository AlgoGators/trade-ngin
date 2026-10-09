// The contract-switch classifier and the back-adjusted series of LOOP_SPEC v6.1 sections 2.1 to 2.3
// (T-ROLLX commit 1), on constructed sequences:
//   * a change bar is the bar whose id differs from the last known id before it; the next consumed
//     bar confirms it a ROLL (id kept: the legs' bars are named) or a FLIP (id reverted: every bar
//     of the sequence, the reverting bar included); a third id extends the pending sequence; a
//     return to an older id after a confirmation is a new roll; an unknown id is not judged;
//   * the adjusted level is the raw close plus the later steps, re-anchored on the latest bar, and
//     can be negative; the adjusted return is the adjusted change over the RAW previous close, 0 on
//     a change bar and the raw simple return everywhere else.
#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

#include <chrono>
#include <random>

#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin::roll_series;

namespace {

std::vector<bool> bits(std::initializer_list<int> v) {
    std::vector<bool> out;
    for (int b : v) out.push_back(b != 0);
    return out;
}

std::vector<std::string> ids(std::initializer_list<const char*> v) {
    return std::vector<std::string>(v.begin(), v.end());
}

}  // namespace

TEST(RollSeries, ARollIsConfirmedOnTheNextConsumedBarAndNamesTheLegBars) {
    const auto f = classify_instrument_changes(ids({"A", "A", "B", "B", "B"}));
    EXPECT_EQ(f.change, bits({0, 0, 1, 0, 0}));
    EXPECT_EQ(f.confirm, bits({0, 0, 0, 1, 0}));
    EXPECT_EQ(f.flip, bits({0, 0, 0, 0, 0}));
    EXPECT_EQ(f.rolled, bits({0, 0, 1, 0, 0}));
    EXPECT_EQ(f.pending, bits({0, 0, 1, 0, 0}));
    EXPECT_EQ(f.leg_from[3], 1) << "the closing leg's close: the last consumed bar before the change";
    EXPECT_EQ(f.leg_to[3], 2) << "the opening leg's close: the change bar";
    EXPECT_EQ(f.held_id, (std::vector<std::string>{"A", "A", "A", "B", "B"}));
    EXPECT_FALSE(f.pending_at_end());
}

TEST(RollSeries, AChangeBarIsPendingUntilTheNextConsumedBar) {
    const auto f = classify_instrument_changes(ids({"A", "A", "B"}));
    EXPECT_EQ(f.change, bits({0, 0, 1}));
    EXPECT_EQ(f.confirm, bits({0, 0, 0}));
    EXPECT_EQ(f.pending, bits({0, 0, 1}));
    EXPECT_TRUE(f.pending_at_end()) << "the symbol is held on the next cycle (D37)";
    EXPECT_EQ(f.held_id[2], "A") << "the position is still in the held contract";
}

TEST(RollSeries, AFlipHasNoLegsAndExcludesBothBars) {
    const auto f = classify_instrument_changes(ids({"A", "A", "B", "A", "A"}));
    EXPECT_EQ(f.change, bits({0, 0, 1, 1, 0})) << "the reverting bar's id differs from the previous bar's too";
    EXPECT_EQ(f.confirm, bits({0, 0, 0, 0, 0}));
    EXPECT_EQ(f.flip, bits({0, 0, 1, 1, 0}));
    EXPECT_EQ(f.rolled, bits({0, 0, 0, 0, 0}));
    EXPECT_EQ(f.pending, bits({0, 0, 1, 0, 0}));
    EXPECT_EQ(f.held_id, (std::vector<std::string>{"A", "A", "A", "A", "A"}));
}

TEST(RollSeries, AThirdIdExtendsThePendingSequenceAndTheKeptIdIsConfirmed) {
    const auto f = classify_instrument_changes(ids({"A", "B", "C", "C"}));
    EXPECT_EQ(f.change, bits({0, 1, 1, 0}));
    EXPECT_EQ(f.pending, bits({0, 1, 1, 0}));
    EXPECT_EQ(f.confirm, bits({0, 0, 0, 1}));
    EXPECT_EQ(f.rolled, bits({0, 0, 1, 0})) << "the change bar into the kept id";
    EXPECT_EQ(f.leg_from[3], 0) << "the closing leg closes the held contract at the last pre-change close";
    EXPECT_EQ(f.leg_to[3], 2);
    EXPECT_EQ(f.held_id[3], "C");
}

TEST(RollSeries, AReturnToAnOlderIdAfterAConfirmationIsANewRoll) {
    const auto f = classify_instrument_changes(ids({"A", "B", "B", "A", "A"}));
    EXPECT_EQ(f.change, bits({0, 1, 0, 1, 0}));
    EXPECT_EQ(f.confirm, bits({0, 0, 1, 0, 1}));
    EXPECT_EQ(f.flip, bits({0, 0, 0, 0, 0})) << "the series switched twice: two rolls, no flip";
    EXPECT_EQ(f.rolled, bits({0, 1, 0, 1, 0}));
    EXPECT_EQ(f.leg_from[4], 2);
    EXPECT_EQ(f.leg_to[4], 3);
    EXPECT_EQ(f.held_id, (std::vector<std::string>{"A", "A", "B", "B", "A"}));
}

TEST(RollSeries, AnUnknownIdIsNotJudgedAndLeavesAPendingChangePending) {
    {
        const auto f = classify_instrument_changes(ids({"A", "", "B", "B"}));
        EXPECT_EQ(f.change, bits({0, 0, 1, 0})) << "judged against the last known id";
        EXPECT_EQ(f.confirm, bits({0, 0, 0, 1}));
        EXPECT_EQ(f.leg_from[3], 1) << "the last consumed bar before the change, id known or not";
    }
    {
        const auto f = classify_instrument_changes(ids({"A", "B", "", "B"}));
        EXPECT_EQ(f.change, bits({0, 1, 0, 0}));
        EXPECT_EQ(f.pending, bits({0, 1, 1, 0})) << "the unknown bar is held with the pending change";
        EXPECT_EQ(f.confirm, bits({0, 0, 0, 1}));
        EXPECT_EQ(f.leg_from[3], 0);
        EXPECT_EQ(f.leg_to[3], 1);
    }
    {
        const auto f = classify_instrument_changes(ids({"", "", "A", "A"}));
        EXPECT_EQ(f.change, bits({0, 0, 0, 0})) << "the first known id is the held id, not a change";
        EXPECT_EQ(f.held_id, (std::vector<std::string>{"", "", "A", "A"}));
    }
    EXPECT_EQ(classify_instrument_changes({}).size(), 0u);
}

TEST(RollSeries, TheAdjustedLevelIsTheRawCloseplusTheLaterStepsAnchoredOnTheLatestBar) {
    const std::vector<double> raw{100.0, 102.0, 80.0, 81.0};
    const auto change = bits({0, 0, 1, 0});
    const auto adjusted = adjusted_levels(raw, change);
    ASSERT_EQ(adjusted.size(), 4u);
    EXPECT_DOUBLE_EQ(adjusted[0], 78.0) << "100 + (80 - 102)";
    EXPECT_DOUBLE_EQ(adjusted[1], 80.0);
    EXPECT_DOUBLE_EQ(adjusted[2], 80.0) << "the latest segment is the raw close";
    EXPECT_DOUBLE_EQ(adjusted[3], 81.0);
    const auto r = adjusted_returns(raw, change);
    ASSERT_EQ(r.size(), 3u);
    EXPECT_DOUBLE_EQ(r[0], 0.02) << "a non-change bar's return is the raw simple return";
    EXPECT_DOUBLE_EQ(r[1], 0.0) << "the change bar's return is excluded (0)";
    EXPECT_DOUBLE_EQ(r[2], 1.0 / 80.0);
}

TEST(RollSeries, ANegativeAdjustedLevelStillGivesTheAdjustedChangeOverTheRawPreviousClose) {
    // A backwardated switch bigger than the level (CL, HO, RB in 2020): the earlier segment's
    // adjusted level goes below zero. The return is the adjusted change over the RAW previous close
    // (section 2.4): never a log, a percentage or a ratio of the adjusted level.
    const std::vector<double> raw{10.0, 12.0, 1.0, 2.0};
    const auto change = bits({0, 0, 1, 0});
    const auto adjusted = adjusted_levels(raw, change);
    EXPECT_DOUBLE_EQ(adjusted[0], -1.0);
    EXPECT_DOUBLE_EQ(adjusted[1], 1.0);
    EXPECT_DOUBLE_EQ(adjusted[2], 1.0);
    EXPECT_DOUBLE_EQ(adjusted[3], 2.0);
    const auto r = adjusted_returns(raw, change);
    EXPECT_DOUBLE_EQ(r[0], 0.2) << "(1 - (-1)) / 10: positive, as the raw move 10 -> 12 is";
    EXPECT_DOUBLE_EQ(r[1], 0.0);
    EXPECT_DOUBLE_EQ(r[2], 1.0);
}

TEST(RollSeries, AFlipPairIsTwoExcludedStepsThatNearlyCancel) {
    const std::vector<double> raw{100.0, 101.0, 130.0, 102.0, 103.0};
    const auto s = build_series(raw, ids({"A", "A", "B", "A", "A"}));
    EXPECT_EQ(s.flags.flip, bits({0, 0, 1, 1, 0}));
    EXPECT_DOUBLE_EQ(s.adjusted[4], 103.0);
    EXPECT_DOUBLE_EQ(s.adjusted[3], 102.0);
    EXPECT_DOUBLE_EQ(s.adjusted[2], 102.0) << "130 + (102 - 130)";
    EXPECT_DOUBLE_EQ(s.adjusted[1], 102.0) << "101 + (102 - 130) + (130 - 101)";
    EXPECT_DOUBLE_EQ(s.adjusted[0], 101.0);
    ASSERT_EQ(s.returns.size(), 4u);
    EXPECT_DOUBLE_EQ(s.returns[0], 0.01);
    EXPECT_DOUBLE_EQ(s.returns[1], 0.0);
    EXPECT_DOUBLE_EQ(s.returns[2], 0.0);
    EXPECT_DOUBLE_EQ(s.returns[3], 1.0 / 102.0);
}

TEST(RollSeries, ASeriesWithNoChangeBarIsTheRawSeriesAndItsSimpleReturns) {
    const std::vector<double> raw{50.0, 51.0, 49.0};
    const auto s = build_series(raw, ids({"A", "A", "A"}));
    EXPECT_EQ(s.adjusted, raw);
    EXPECT_DOUBLE_EQ(s.returns[0], 0.02);
    EXPECT_DOUBLE_EQ(s.returns[1], -2.0 / 51.0);
    const auto t = build_series(raw, ids({"", "", ""}));
    EXPECT_EQ(t.adjusted, raw) << "no id anywhere (an equity): nothing is a change";
}

TEST(RollSeries, MisalignedInputsAreRefused) {
    EXPECT_THROW(build_series({1.0, 2.0}, ids({"A"})), std::invalid_argument);
    EXPECT_THROW(adjusted_levels({1.0, 2.0}, bits({0})), std::invalid_argument);
    EXPECT_THROW(adjusted_returns({1.0, 2.0}, bits({0})), std::invalid_argument);
    EXPECT_TRUE(adjusted_returns({1.0}, bits({0})).empty());
}

TEST(RollSeries, TheTrackerReproducesTheBatchClassifierBarByBar) {
    std::mt19937 rng(20261002);
    const std::vector<std::string> alphabet{"A", "B", "C", ""};
    for (int trial = 0; trial < 300; ++trial) {
        const int n = 1 + static_cast<int>(rng() % 40);
        std::vector<std::string> seq;
        std::vector<double> closes;
        std::string cur = "A";
        for (int t = 0; t < n; ++t) {
            const unsigned r = rng() % 10;
            if (r < 2) cur = alphabet[rng() % 4];  // a switch, a third id or an unknown bar
            seq.push_back(cur);
            closes.push_back(100.0 + static_cast<double>(t));
        }
        const auto batch = classify_instrument_changes(seq);
        RollTracker tracker;
        for (int t = 0; t < n; ++t) {
            const auto st = tracker.add(seq[t], closes[t]);
            SCOPED_TRACE("trial " + std::to_string(trial) + " bar " + std::to_string(t));
            EXPECT_EQ(st.change, batch.change[t]);
            EXPECT_EQ(st.confirm, batch.confirm[t]);
            // The batch marks every bar of a flip sequence (retroactively, once the reversion is
            // seen); the tracker, bar by bar, can only mark the reverting bar itself.
            EXPECT_EQ(st.flip, batch.flip[t] && !batch.pending[t]);
            EXPECT_EQ(st.pending, batch.pending[t]);
            EXPECT_EQ(st.held_id, batch.held_id[t]);
            if (st.confirm) {
                EXPECT_EQ(st.last_close_before_change, closes[static_cast<size_t>(batch.leg_from[t])]);
                EXPECT_EQ(st.change_bar_close, closes[static_cast<size_t>(batch.leg_to[t])]);
            }
        }
        EXPECT_EQ(tracker.pending(), batch.pending_at_end());
    }
}

TEST(RollSeries, TheTrackerNamesTheLegsOfAConfirmedRoll) {
    RollTracker t;
    t.add("A", 3.30);
    t.add("A", 3.376);
    auto s = t.add("B", 3.965);
    EXPECT_TRUE(s.change && s.pending && !s.confirm);
    EXPECT_EQ(s.held_id, "A");
    s = t.add("B", 3.822);
    EXPECT_TRUE(s.confirm);
    EXPECT_EQ(s.previous_held_id, "A");
    EXPECT_EQ(s.held_id, "B");
    EXPECT_DOUBLE_EQ(s.last_close_before_change, 3.376) << "the closing leg: the last pre-change close";
    EXPECT_DOUBLE_EQ(s.change_bar_close, 3.965) << "the opening leg: the change bar's close";
    EXPECT_EQ(s.bars_pending, 1);
}

// LOOP_SPEC v6.1 section 2.2 (LEAD-2 2026-10-02, code review D2): an id-less bar inside a pending
// roll is not judged, leaves the roll pending and HOLDS the symbol; the hold ends on the confirm.
TEST(RollSeries, AnIdLessBarInsideAPendingRollStillHolds) {
    RollTracker t;
    EXPECT_FALSE(t.add("A", 100.0).holds());
    const auto change = t.add("B", 103.0);
    EXPECT_TRUE(change.change);
    EXPECT_TRUE(change.holds());
    const auto idless = t.add("", 104.0);
    EXPECT_FALSE(idless.change) << "not judged";
    EXPECT_TRUE(idless.pending);
    EXPECT_TRUE(idless.holds()) << "the roll is still pending: the symbol is held (the parent's `change` test missed it)";
    const auto confirm = t.add("B", 105.0);
    EXPECT_TRUE(confirm.confirm);
    EXPECT_FALSE(confirm.holds());
    EXPECT_EQ(confirm.held_id, "B");
    EXPECT_DOUBLE_EQ(confirm.last_close_before_change, 100.0);
    EXPECT_DOUBLE_EQ(confirm.change_bar_close, 103.0);
}

TEST(RollSeries, BothBarsOfAFlipHold) {
    RollTracker t;
    t.add("A", 100.0);
    EXPECT_TRUE(t.add("B", 90.0).holds());
    const auto back = t.add("A", 101.0);
    EXPECT_TRUE(back.flip);
    EXPECT_TRUE(back.change);
    EXPECT_TRUE(back.holds()) << "the reverting bar is pending until the next consumed bar (L-04)";
    EXPECT_FALSE(t.add("A", 102.0).holds());
}

// K-01 in the roll status: the status is taken on the CONSUMED bars; a withheld print carrying
// another contract's id is in no sequence, so it is neither a change nor a confirm.
TEST(RollSeries, TheRollStatusOfTheConsumedBarsNeverSeesAWithheldPrint) {
    auto bar = [](int d, double c, const std::string& id) {
        trade_ngin::Bar b(trade_ngin::Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)), c, c, c, c, 1000.0, "XA.v.0");
        b.instrument_id = id;
        return b;
    };
    const std::vector<trade_ngin::Bar> loaded = {bar(0, 100, "A"), bar(1, 50, "B"), bar(2, 101, "A")};
    const std::vector<trade_ngin::Bar> consumed = {bar(0, 100, "A"), bar(2, 101, "A")};  // day 1 withheld
    EXPECT_TRUE(roll_status_of(loaded).at("XA.v.0").flip)
        << "walked over the loaded window the corrupt print makes a flip (the T-ROLLX break)";
    const auto st = roll_status_of(consumed).at("XA.v.0");
    EXPECT_FALSE(st.change);
    EXPECT_FALSE(st.flip);
    EXPECT_FALSE(st.holds());
    EXPECT_EQ(st.held_id, "A");
}
