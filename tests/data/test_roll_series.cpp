// The contract-switch classifier and the back-adjusted series, on constructed sequences:
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
    // the per-contract forms (ChangeFlags with the roll fields' closes)
    const ChangeFlags three = classify_instrument_changes(ids({"A", "A", "B"}), RollFields(3));
    EXPECT_THROW(return_bases({1.0, 2.0}, three), std::invalid_argument);
    EXPECT_THROW(adjusted_levels({1.0, 2.0}, three), std::invalid_argument);
    EXPECT_THROW(adjusted_returns({1.0, 2.0}, three), std::invalid_argument);
    EXPECT_EQ(return_bases({1.0, 2.0, 3.0}, three), (std::vector<double>{1.0, 2.0, 3.0}));
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

// An id-less bar inside a pending
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

// A withheld bar in the roll status: the status is taken on the CONSUMED bars; a withheld print carrying
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

// ------------------------------------------------------------------------------------------------
// The per-contract source's roll field (Bar::roll_from). On a roll bar the step is the
// new close minus the OLD contract's close on the same bar and the return is the old contract's own.
// ------------------------------------------------------------------------------------------------

namespace {

using trade_ngin::RollFrom;
using trade_ngin::RollFromKind;

std::optional<RollFrom> from(double close, const char* id, RollFromKind kind = RollFromKind::KNOWN,
                             const char* date = "") {
    return RollFrom{close, id, kind, date};
}

}  // namespace

TEST(RollSeriesPerContract, ARollKnownOnItsBarIsConfirmedThereAndItsReturnIsTheOldContracts) {
    // A: 100, 101, 102 (roll bar: A's own close 102, B's 110), then B 111, 112.
    const std::vector<double> raw{100, 101, 110, 111, 112};
    const RollFields rf{std::nullopt, std::nullopt, from(102, "A"), from(103, "A", RollFromKind::AFTER_ROLL),
                        std::nullopt};
    const auto s = build_series(raw, ids({"A", "A", "B", "B", "B"}), rf);
    EXPECT_EQ(s.flags.change, bits({0, 0, 1, 0, 0}));
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 1, 0, 0})) << "known on its bar: no hold, the legs here";
    EXPECT_EQ(s.flags.pending, bits({0, 0, 0, 0, 0}));
    EXPECT_EQ(s.flags.leg_to[2], 2);
    EXPECT_EQ(*s.flags.old_close[2], 102.0) << "the closing leg: the old contract's close on the bar";
    EXPECT_EQ(s.flags.held_id, (std::vector<std::string>{"A", "A", "B", "B", "B"}));
    // The step is 110 - 102 = 8; the earlier levels carry it.
    EXPECT_EQ(s.adjusted, (std::vector<double>{108, 109, 110, 111, 112}));
    ASSERT_EQ(s.returns.size(), 4u);
    EXPECT_DOUBLE_EQ(s.returns[1], (102.0 - 101.0) / 101.0) << "the roll bar: A's own move";
    EXPECT_DOUBLE_EQ(s.returns[2], (111.0 - 110.0) / 110.0);
    EXPECT_EQ(s.raw, raw);
    // The tracker agrees and its status prices the bar's move.
    RollTracker t;
    RollTracker::Status st;
    for (size_t i = 0; i < raw.size(); ++i) {
        const auto prev = i ? raw[i - 1] : 0.0;
        st = t.add(ids({"A", "A", "B", "B", "B"})[i], raw[i], rf[i]);
        if (i == 2) {
            EXPECT_TRUE(st.change);
            EXPECT_TRUE(st.confirm);
            EXPECT_FALSE(st.holds());
            EXPECT_EQ(st.previous_held_id, "A");
            EXPECT_EQ(st.last_close_before_change, 102.0);
            EXPECT_EQ(st.change_bar_close, 110.0);
            EXPECT_EQ(st.move(raw[i], prev), 1.0);
        }
        if (i == 3) {
            EXPECT_FALSE(st.change) << "the AFTER_ROLL field is not read when the roll bar was consumed";
            EXPECT_EQ(st.move(raw[i], prev), 1.0);
        }
    }
}

TEST(RollSeriesPerContract, AVendorSwitchTheNextBarConfirmsIsHeldThenLeggedAtTheSwitchBarsCloses) {
    // The switch bar is A's own (102), the field naming the target B (110); the next bar is B's.
    const std::vector<double> raw{100, 101, 102, 111};
    const RollFields rf{std::nullopt, std::nullopt, from(110, "B", RollFromKind::AWAITS_NEXT),
                        from(104, "A", RollFromKind::AFTER_ROLL)};
    const auto s = build_series(raw, ids({"A", "A", "A", "B"}), rf);
    EXPECT_EQ(s.flags.change, bits({0, 0, 0, 1}));
    EXPECT_EQ(s.flags.pending, bits({0, 0, 1, 0}));
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 0, 1}));
    EXPECT_EQ(s.flags.rolled, bits({0, 0, 1, 0}));
    EXPECT_EQ(s.flags.leg_from[3], 2) << "the closing leg: the switch bar's own close, A's 102";
    EXPECT_EQ(*s.flags.leg_opening_price[3], 110.0) << "the opening leg: B's close on the switch bar";
    EXPECT_EQ(*s.flags.base_close[2], 110.0);
    EXPECT_EQ(s.raw, raw) << "every level is the held contract's close";
    // The step is B's 110 less A's 102 = 8, taken between the switch bar and the next one.
    EXPECT_EQ(s.adjusted, (std::vector<double>{108, 109, 110, 111}));
    EXPECT_DOUBLE_EQ(s.returns[1], (102.0 - 101.0) / 101.0) << "the switch bar: A's own move";
    EXPECT_DOUBLE_EQ(s.returns[2], (111.0 - 110.0) / 110.0) << "the next bar: B's move from 110";
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101);
    const auto sw = t.add("A", 102, rf[2]);
    EXPECT_TRUE(sw.holds()) << "a run whose bars end on the switch bar holds the symbol";
    EXPECT_FALSE(sw.change);
    EXPECT_EQ(sw.held_id, "A");
    EXPECT_FALSE(sw.mark_close.has_value());
    EXPECT_EQ(sw.move(102, 101), 1.0);
    const auto c = t.add("B", 111, rf[3]);
    EXPECT_TRUE(c.confirm);
    EXPECT_FALSE(c.holds());
    EXPECT_EQ(c.previous_held_id, "A");
    EXPECT_EQ(c.last_close_before_change, 102.0);
    EXPECT_EQ(c.change_bar_close, 110.0);
    EXPECT_EQ(c.held_id, "B");
    EXPECT_EQ(*c.previous_close, 110.0);
    EXPECT_EQ(c.move(111, 102), 1.0) << "from B's close on the switch bar, not A's";
}

TEST(RollSeriesPerContract, AVendorSwitchTheNextBarDoesNotConfirmChangesNothing) {
    // A stray: the switch bar is A's own bar (102), the field naming B; the next bar is A's again.
    const std::vector<double> raw{100, 101, 102, 103, 104};
    const RollFields rf{std::nullopt, std::nullopt, from(110, "B", RollFromKind::AWAITS_NEXT),
                        std::nullopt, std::nullopt};
    const auto s = build_series(raw, ids({"A", "A", "A", "A", "A"}), rf);
    EXPECT_EQ(s.flags.change, bits({0, 0, 0, 0, 0}));
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 0, 0, 0}));
    EXPECT_EQ(s.flags.flip, bits({0, 0, 0, 0, 0})) << "not a flip: nothing rolled";
    EXPECT_FALSE(s.flags.base_close[2].has_value());
    // Exactly the series of A's own bars, the levels included.
    const auto own = build_series(raw, ids({"A", "A", "A", "A", "A"}));
    EXPECT_EQ(s.raw, own.raw);
    EXPECT_EQ(s.adjusted, own.adjusted);
    EXPECT_EQ(s.returns, own.returns);
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101);
    EXPECT_TRUE(t.add("A", 102, rf[2]).holds());
    const auto back = t.add("A", 103);
    EXPECT_FALSE(back.change);
    EXPECT_FALSE(back.flip);
    EXPECT_FALSE(back.holds());
    EXPECT_FALSE(back.previous_close.has_value());
    EXPECT_EQ(back.move(103, 102), 1.0);
    EXPECT_EQ(back.held_id, "A");
}

TEST(RollSeriesPerContract, AWithheldRollBarIsBookedOnTheNextConsumedBarAtItsCloses) {
    // The consumer withheld the roll bar: its next consumed bar is B's with A's close of that date.
    const std::vector<double> raw{100, 101, 111, 112};
    const RollFields rf{std::nullopt, std::nullopt, from(103, "A", RollFromKind::AFTER_ROLL),
                        std::nullopt};
    const auto s = build_series(raw, ids({"A", "A", "B", "B"}), rf);
    EXPECT_EQ(s.flags.change, bits({0, 0, 1, 0}));
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 1, 0}));
    EXPECT_EQ(*s.flags.old_close[2], 103.0);
    EXPECT_DOUBLE_EQ(s.returns[1], (103.0 - 101.0) / 101.0) << "the old contract's move up to it";
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101);
    const auto st = t.add("B", 111, rf[2]);
    EXPECT_TRUE(st.confirm);
    EXPECT_FALSE(st.holds());
    EXPECT_EQ(st.last_close_before_change, 103.0);
    EXPECT_EQ(st.change_bar_close, 111.0);
}

TEST(RollSeriesPerContract, WithNoFieldTheFlagsAndTheSeriesAreTodays) {
    const auto seq = ids({"A", "A", "B", "A", "C", "C", "", "C"});
    const std::vector<double> raw{1, 2, 3, 4, 5, 6, 7, 8};
    const RollFields none(seq.size());
    const auto a = build_series(raw, seq);
    const auto b = build_series(raw, seq, none);
    EXPECT_EQ(a.flags.change, b.flags.change);
    EXPECT_EQ(a.flags.confirm, b.flags.confirm);
    EXPECT_EQ(a.flags.flip, b.flags.flip);
    EXPECT_EQ(a.flags.pending, b.flags.pending);
    EXPECT_EQ(a.flags.leg_from, b.flags.leg_from);
    EXPECT_EQ(a.flags.leg_to, b.flags.leg_to);
    EXPECT_EQ(a.adjusted, b.adjusted);
    EXPECT_EQ(a.returns, b.returns);
    EXPECT_EQ(a.adjusted, adjusted_levels(raw, classify_instrument_changes(seq).change));
    EXPECT_EQ(a.returns, adjusted_returns(raw, classify_instrument_changes(seq).change));
    EXPECT_THROW(classify_instrument_changes(seq, RollFields(3)), std::invalid_argument);
}

TEST(RollSeriesPerContract, TwoRollsResolvedOnOneBarStopTheTracker) {
    RollTracker t;
    t.add("A", 100);
    t.add("A", 102, from(110, "B", RollFromKind::AWAITS_NEXT));
    EXPECT_THROW(t.add("C", 120, from(111, "B")), std::runtime_error);
}

// Per-contract sequences as the source makes them (rolls forward only, KNOWN, AWAITS_NEXT kept or
// not, the AFTER_ROLL field on the next bar, bars a consumer withheld dropped): the tracker, bar by
// bar, equals the batch classifier.
TEST(RollSeriesPerContract, TheTrackerReproducesTheBatchClassifierOnPerContractSequences) {
    std::mt19937 rng(20261023);
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<std::string> seq;
        std::vector<double> closes;
        RollFields rf;
        int held = 0;  // contract index; ids "c0", "c1", ...
        bool after = false;
        int left = 0;
        std::string left_on;
        const int n = 2 + static_cast<int>(rng() % 40);
        for (int t = 0; t < n; ++t) {
            const unsigned r = rng() % 12;
            const std::string h = "c" + std::to_string(held);
            const double base = 100.0 + 10.0 * held + t;
            std::optional<RollFrom> f;
            std::string id = h;
            double close = base;
            const std::string day = "d" + std::to_string(100 + t);  // the roll's date
            if (r == 0 && t > 0) {  // a known roll
                f = RollFrom{base - 3.0, h, RollFromKind::KNOWN, day};
                left = held++;
                left_on = day;
                id = "c" + std::to_string(held);
                close = base + 10.0;
                after = true;
            } else if (r == 1 && t > 0) {  // a vendor switch, kept by the next bar or not: the
                // held contract's own bar, the field naming the next contract
                f = RollFrom{base + 10.0, "c" + std::to_string(held + 1), RollFromKind::AWAITS_NEXT,
                             day};
                if (trial % 2) {
                    // "vendor" (every other trial): the target's own bar, the field naming
                    // the held contract
                    f = RollFrom{base, h, RollFromKind::AWAITS_NEXT_VENDOR, day};
                    id = "c" + std::to_string(held + 1);
                    close = base + 10.0;
                }
                if (rng() % 2) {
                    left = held++;
                    left_on = day;
                    after = true;
                }
            } else if (after) {
                f = RollFrom{base - 4.0, "c" + std::to_string(left), RollFromKind::AFTER_ROLL, left_on};
                after = rng() % 3 == 0;  // the field rides the bars after a roll
            }
            // a bar the consumer withheld: an ordinary bar, or a known roll bar (its AFTER_ROLL
            // field then books it on the next consumed bar)
            const bool withheld =
                t > 0 && ((r == 2 && !f) || (f && f->kind == RollFromKind::KNOWN && rng() % 3 == 0));
            if (withheld) continue;
            seq.push_back(id);
            closes.push_back(close);
            rf.push_back(f);
        }
        const auto batch = classify_instrument_changes(seq, rf);
        RollTracker tracker;
        for (size_t t = 0; t < seq.size(); ++t) {
            SCOPED_TRACE("trial " + std::to_string(trial) + " bar " + std::to_string(t));
            RollTracker::Status st;
            try {
                st = tracker.add(seq[t], closes[t], rf[t]);
            } catch (const std::runtime_error&) {
                break;  // two rolls on one bar: refused (TwoRollsResolvedOnOneBarStopTheTracker)
            }
            EXPECT_EQ(st.change, batch.change[t]);
            EXPECT_EQ(st.confirm, batch.confirm[t]);
            EXPECT_EQ(st.pending, batch.pending[t]);
            EXPECT_EQ(st.held_id, batch.held_id[t]);
            EXPECT_EQ(st.mark_close.has_value(), batch.old_close[t].has_value());
            EXPECT_EQ(st.previous_close.has_value(), t > 0 && batch.base_close[t - 1].has_value());
            if (st.confirm) {
                const size_t to = static_cast<size_t>(batch.leg_to[t]);
                EXPECT_EQ(st.change_bar_close,
                          batch.leg_opening_price[t] ? *batch.leg_opening_price[t] : closes[to]);
                EXPECT_EQ(st.last_close_before_change,
                          batch.old_close[to] ? *batch.old_close[to]
                                              : closes[static_cast<size_t>(batch.leg_from[t])]);
            }
        }
    }
}

TEST(RollSeriesPerContract, ARollBarAndTheBarAfterItBothWithheldAreBookedOnTheFirstConsumedBar) {
    // The consumer withheld the roll bar and the bar after it; the source carries the
    // old contract's close on every bar after a roll, so the first consumed bar books the roll, the
    // old contract's own move up to it (101 -> 105) and both legs at its closes.
    const std::vector<double> raw{100, 101, 112, 113};
    const RollFields rf{std::nullopt, std::nullopt, from(105, "A", RollFromKind::AFTER_ROLL),
                        from(106, "A", RollFromKind::AFTER_ROLL)};
    const auto s = build_series(raw, ids({"A", "A", "B", "B"}), rf);
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 1, 0}));
    EXPECT_DOUBLE_EQ(s.returns[1], (105.0 - 101.0) / 101.0) << "not 0: the old contract moved";
    EXPECT_DOUBLE_EQ(s.returns[2], (113.0 - 112.0) / 112.0) << "the second field is not read";
}

// The 6L 2013-09-11 case. The consumer consumed an unconfirmed vendor switch
// into B on 09-06 (A's own bar, the field naming B), withheld every bar after it, the real roll into B on
// 09-11 included, and consumes 09-18, B's bar with A's close of 09-18 in an AFTER_ROLL field dated 09-11.
// That field follows a later roll, not the switch it awaits: the roll is booked on 09-18 at its two
// closes and the bar's move is A's own up to it, 104 - 101 (not confirmed at 09-06's closes, nor the
// move from B's 110).
TEST(RollSeriesPerContract, AnAwaitedSwitchIsNotConfirmedByALaterRollIntoTheSameTarget) {
    const std::vector<double> raw{100, 101, 115};
    const RollFields rf{std::nullopt, from(110, "B", RollFromKind::AWAITS_NEXT, "2013-09-06"),
                        from(104, "A", RollFromKind::AFTER_ROLL, "2013-09-11")};
    const auto s = build_series(raw, ids({"A", "A", "B"}), rf);
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 1}));
    EXPECT_EQ(s.flags.rolled, bits({0, 0, 1})) << "the roll is 09-18's, not the switch bar's";
    EXPECT_FALSE(s.flags.base_close[1].has_value());
    EXPECT_EQ(*s.flags.old_close[2], 104.0);
    EXPECT_EQ(s.flags.leg_to[2], 2);
    EXPECT_DOUBLE_EQ(s.returns[1], (104.0 - 101.0) / 101.0) << "A's own move up to 09-18";
    RollTracker t;
    t.add("A", 100);
    EXPECT_TRUE(t.add("A", 101, rf[1]).holds());
    const auto st = t.add("B", 115, rf[2]);
    EXPECT_TRUE(st.confirm);
    EXPECT_FALSE(st.holds());
    EXPECT_EQ(st.previous_held_id, "A");
    EXPECT_EQ(*st.mark_close, 104.0);
    EXPECT_FALSE(st.previous_close.has_value());
    EXPECT_EQ(st.last_close_before_change, 104.0) << "the closing leg: A's close on 09-18";
    EXPECT_EQ(st.change_bar_close, 115.0) << "the opening leg: B's close on 09-18";
    EXPECT_EQ(st.move(115, 101), 3.0);
    EXPECT_EQ(st.held_id, "B");
}

// The same bars with the field dated the switch bar's date: the awaited switch itself, confirmed at the
// switch bar's closes.
TEST(RollSeriesPerContract, AnAwaitedSwitchIsConfirmedByAFieldDatedItsOwnBar) {
    const std::vector<double> raw{100, 101, 115};
    const RollFields rf{std::nullopt, from(110, "B", RollFromKind::AWAITS_NEXT, "2013-09-06"),
                        from(104, "A", RollFromKind::AFTER_ROLL, "2013-09-06")};
    const auto s = build_series(raw, ids({"A", "A", "B"}), rf);
    EXPECT_EQ(s.flags.confirm, bits({0, 0, 1}));
    EXPECT_EQ(s.flags.rolled, bits({0, 1, 0}));
    EXPECT_EQ(*s.flags.base_close[1], 110.0);
    EXPECT_DOUBLE_EQ(s.returns[1], (115.0 - 110.0) / 110.0);
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101, rf[1]);
    const auto st = t.add("B", 115, rf[2]);
    EXPECT_TRUE(st.confirm);
    EXPECT_EQ(*st.previous_close, 110.0);
    EXPECT_EQ(st.last_close_before_change, 101.0);
    EXPECT_EQ(st.change_bar_close, 110.0);
}

// With "vendor" the switch bar is the target's own (B 110), the field naming the
// held contract (A 102). Confirmed by the next bar: legs at the switch bar's closes, the next move from
// B's 110. Not confirmed: the next move from A's 102. Both exactly as the "held" representation books.
TEST(RollSeriesPerContract, AVendorSettingSwitchBarBooksWhatTheHeldSettingBooks) {
    for (const bool confirmed : {true, false}) {
        SCOPED_TRACE(confirmed ? "confirmed" : "not confirmed");
        const double next = confirmed ? 111.0 : 103.0;
        const std::vector<double> raw_h{100, 101, 102, next}, raw_v{100, 101, 110, next};
        const auto ids_h = ids({"A", "A", "A", confirmed ? "B" : "A"});
        const auto ids_v = ids({"A", "A", "B", confirmed ? "B" : "A"});
        const std::optional<RollFrom> after =
            confirmed ? from(104, "A", RollFromKind::AFTER_ROLL, "d2") : std::nullopt;
        const RollFields rf_h{std::nullopt, std::nullopt, from(110, "B", RollFromKind::AWAITS_NEXT, "d2"), after};
        const RollFields rf_v{std::nullopt, std::nullopt,
                              from(102, "A", RollFromKind::AWAITS_NEXT_VENDOR, "d2"), after};
        const auto h = build_series(raw_h, ids_h, rf_h);
        const auto v = build_series(raw_v, ids_v, rf_v);
        EXPECT_EQ(h.returns, v.returns);
        EXPECT_EQ(h.adjusted, v.adjusted);
        EXPECT_EQ(h.flags.held_id, v.flags.held_id);
        EXPECT_EQ(h.flags.confirm, v.flags.confirm);
        EXPECT_EQ(h.flags.pending, v.flags.pending);
        EXPECT_EQ(h.flags.rolled, v.flags.rolled);
        EXPECT_DOUBLE_EQ(v.returns[1], (102.0 - 101.0) / 101.0) << "the switch bar: A's own move";
        RollTracker th, tv;
        for (size_t t = 0; t < raw_h.size(); ++t) {
            const auto a = th.add(ids_h[t], raw_h[t], rf_h[t]);
            const auto b = tv.add(ids_v[t], raw_v[t], rf_v[t]);
            if (t > 0) EXPECT_EQ(a.move(raw_h[t], raw_h[t - 1]), b.move(raw_v[t], raw_v[t - 1])) << t;
            EXPECT_EQ(a.holds(), b.holds()) << t;
            EXPECT_EQ(a.held_id, b.held_id) << t;
            EXPECT_EQ(a.confirm, b.confirm) << t;
            if (a.confirm) {
                EXPECT_EQ(a.last_close_before_change, b.last_close_before_change);
                EXPECT_EQ(a.change_bar_close, b.change_bar_close);
            }
        }
    }
}
