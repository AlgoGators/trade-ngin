// tests/live/test_live_roll_legs.cpp
//
// LOOP_SPEC v6.2 section 6.5 in the live runners (T-ROLLX-FIX commit 4, B8; code review D3, L-09):
// the legs are booked by STATE. A run legs every roll a consumed bar confirmed since the contract
// recorded on the symbol's stored T-1 positions row (live_rolls_by_state), never the rolls of a
// calendar span, so each roll is booked once and a late (back-filled) confirming bar is legged on
// the run that first consumes it. The legs' ids carry the CONFIRMING bar's date.
//
// The runs below are driven as the runner drives them: each run consumes the bars dated before it
// that exist at that moment, reads the contract on the stored T-1 row, legs, and stores on the row
// the contract held after its last consumed bar (PHASE 5 and the Day T row write the same id).

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"
#include "trade_ngin/live/session_book_gate.hpp"

using namespace trade_ngin;

namespace {

Timestamp at(const std::string& ymd) {
    std::tm tm{};
    tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(ymd.substr(8, 2));
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
std::string plus(const std::string& ymd, int n) {
    return core::format_utc_date(at(ymd) + std::chrono::hours(24 * n));
}

Bar bar(const std::string& symbol, const std::string& ymd, double close, const std::string& id) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = at(ymd);
    b.open = Decimal(close);
    b.high = Decimal(close * 1.01);
    b.low = Decimal(close * 0.99);
    b.close = Decimal(close);
    b.volume = 100000.0;
    b.instrument_id = id;
    return b;
}

// ZM.v.0 on the stage-3 clone (the adversary's B-8 case): no bar 04-17 and 04-21..04-24.
//   04-16 332.20 (OLD)   04-20 321.10 (NEW, the change bar)   04-27 328.00 (NEW, confirms)
//   04-28 328.80         04-29 324.10
const std::string kZm = "ZM.v.0";
std::vector<Bar> zm_bars() {
    return {bar(kZm, "2026-04-15", 331.00, "OLD"), bar(kZm, "2026-04-16", 332.20, "OLD"),
            bar(kZm, "2026-04-20", 321.10, "NEW"), bar(kZm, "2026-04-27", 328.00, "NEW"),
            bar(kZm, "2026-04-28", 328.80, "NEW"), bar(kZm, "2026-04-29", 324.10, "NEW")};
}

// One run of the runner's composition for a single held symbol (long 1, multiplier 100).
struct LegRun {
    std::vector<ConfirmedRoll> rolls;
    bool unplaced{false};
    double pnl{0.0};           // the move the T-1 row books
    std::string stored_after;  // the contract the run leaves on the rows
};
LegRun run_on(const std::vector<Bar>& all, const std::set<std::string>& missing, const std::string& run_date,
           const std::string& recorded) {
    LegRun out;
    const std::string t1 = plus(run_date, -1);
    std::vector<Bar> consumed;
    std::unordered_map<std::string, double> raw_t1, raw_t2;
    const Bar* before = nullptr;
    for (const auto& b : all) {
        const std::string d = core::format_utc_date(b.timestamp);
        if (d >= run_date || missing.count(d)) continue;
        consumed.push_back(b);
        if (d == t1) {
            raw_t1[b.symbol] = static_cast<double>(b.close);
            if (before) raw_t2[b.symbol] = static_cast<double>(before->close);
        }
        before = &b;
    }
    const auto state = live_rolls_by_state(consumed, {{kZm, recorded}}, t1);
    out.rolls = state.rolls;
    out.unplaced = !state.unplaced.empty();
    const auto status = roll_series::roll_status_of(consumed);
    out.stored_after = status.count(kZm) ? status.at(kZm).held_id : recorded;
    const auto st = consumed_t1_settlement(consumed, t1, status, {}, raw_t2, raw_t1, state.late);
    const auto to = st.t1_close_prices.find(kZm);
    const auto from = st.t2_close_prices.find(kZm);
    if (to != st.t1_close_prices.end() && from != st.t2_close_prices.end() &&
        !st.zero_pnl_symbols.count(kZm)) {
        out.pnl = 1.0 * 100.0 * (to->second - from->second);
    }
    return out;
}

}  // namespace

// The control: every bar present on its own run. The 04-28 run (T-1 = the confirming bar) legs the
// roll and books the confirming bar's move against the change bar's close.
TEST(LiveRollLegs, ARollIsLeggedOnTheRunWhoseT1ConfirmsIt) {
    const auto all = zm_bars();
    const LegRun r27 = run_on(all, {}, "2026-04-27", "OLD");  // T-1 04-26: no bar; pending since 04-20
    EXPECT_TRUE(r27.rolls.empty());
    EXPECT_EQ(r27.stored_after, "OLD") << "the outgoing contract while the change is pending";
    const LegRun r28 = run_on(all, {}, "2026-04-28", r27.stored_after);
    ASSERT_EQ(r28.rolls.size(), 1u);
    EXPECT_EQ(r28.rolls[0].confirm_date, "2026-04-27");
    EXPECT_EQ(r28.rolls[0].outgoing_id, "OLD");
    EXPECT_EQ(r28.rolls[0].incoming_id, "NEW");
    EXPECT_NEAR(r28.rolls[0].closing_price, 332.20, 1e-9);
    EXPECT_NEAR(r28.rolls[0].opening_price, 321.10, 1e-9);
    EXPECT_NEAR(r28.pnl, 100.0 * (328.00 - 321.10), 1e-9) << "690.00";
    EXPECT_EQ(r28.stored_after, "NEW");
    const LegRun r29 = run_on(all, {}, "2026-04-29", r28.stored_after);
    EXPECT_TRUE(r29.rolls.empty()) << "booked once";
    EXPECT_NEAR(r29.pnl, 100.0 * (328.80 - 328.00), 1e-9) << "80.00";
}

// B8, the adversary's LATE case: the 04-27 bar is a feed hole on its own T-1 run (04-28) and is
// back-filled before the 04-29 run. The 04-29 run finds the roll confirmed after the recorded
// contract: it legs it once, ids dated 04-27, and books the confirming bar's move with its own T-1
// move, 100 x (328.80 - 321.10) = 770.00 = 690.00 + 80.00. A calendar span (04-27, 04-28] never
// legged it and booked 80.00.
TEST(LiveRollLegs, ALateConfirmingBarIsLeggedOnTheRunThatFirstConsumesItAndItsMoveIsBooked) {
    const auto all = zm_bars();
    const LegRun r28 = run_on(all, {"2026-04-27"}, "2026-04-28", "OLD");
    EXPECT_TRUE(r28.rolls.empty()) << "no confirming bar yet";
    EXPECT_NEAR(r28.pnl, 0.0, 1e-12);
    EXPECT_EQ(r28.stored_after, "OLD");
    const LegRun r29 = run_on(all, {}, "2026-04-29", r28.stored_after);
    ASSERT_EQ(r29.rolls.size(), 1u) << "the roll confirmed by the late bar is legged";
    EXPECT_EQ(r29.rolls[0].confirm_date, "2026-04-27");
    EXPECT_EQ("EXEC_" + kZm + "_" + compact_date(r29.rolls[0].confirm_date) + "_RC",
              "EXEC_ZM.v.0_20260427_RC");
    EXPECT_NEAR(r29.rolls[0].closing_price, 332.20, 1e-9);
    EXPECT_NEAR(r29.rolls[0].opening_price, 321.10, 1e-9);
    EXPECT_NEAR(r29.pnl, 770.00, 1e-9) << "the confirming bar's 690.00 and T-1's 80.00";
    EXPECT_EQ(r29.stored_after, "NEW");
    const LegRun r30 = run_on(all, {}, "2026-04-30", r29.stored_after);
    EXPECT_TRUE(r30.rolls.empty()) << "booked once";
    EXPECT_NEAR(r30.pnl, 100.0 * (324.10 - 328.80), 1e-9);
}

// A hole on the confirming bar that is never back-filled before a later bar of the new contract:
// that later bar confirms on its own run, and the back-fill afterwards owes nothing.
TEST(LiveRollLegs, ALaterBarConfirmsWhenTheHoleStaysAndTheBackFillOwesNothing) {
    const auto all = zm_bars();
    const LegRun r28 = run_on(all, {"2026-04-27"}, "2026-04-28", "OLD");
    const LegRun r29 = run_on(all, {"2026-04-27"}, "2026-04-29", r28.stored_after);
    ASSERT_EQ(r29.rolls.size(), 1u);
    EXPECT_EQ(r29.rolls[0].confirm_date, "2026-04-28") << "the 04-28 bar is the confirming bar here";
    EXPECT_NEAR(r29.pnl, 100.0 * (328.80 - 321.10), 1e-9);
    const LegRun r30 = run_on(all, {}, "2026-04-30", r29.stored_after);  // the 04-27 bar back-filled
    EXPECT_TRUE(r30.rolls.empty()) << "the stored row is already in the new contract";
    EXPECT_NEAR(r30.pnl, 100.0 * (324.10 - 328.80), 1e-9);
}

// A re-run of the date: the first run re-wrote the T-1 row with the new contract, so the runner
// passes the contract its stored legs rolled out of; the same roll is found again (the sweep clears
// the first run's rows). With the re-written row alone, the roll its T-1 bar confirms is still owed
// (the replay of a run that stopped before storing its legs).
TEST(LiveRollLegs, AReRunAndAReplayAfterAStoppedRunLegTheSameRoll) {
    const auto all = zm_bars();
    const LegRun rerun_from_legs = run_on(all, {}, "2026-04-28", "OLD");  // get_stored_roll_contracts
    const LegRun replay_from_row = run_on(all, {}, "2026-04-28", "NEW");  // the re-written T-1 row
    for (const LegRun* r : {&rerun_from_legs, &replay_from_row}) {
        ASSERT_EQ(r->rolls.size(), 1u);
        EXPECT_EQ(r->rolls[0].confirm_date, "2026-04-27");
        EXPECT_NEAR(r->pnl, 690.00, 1e-9);
    }
    // The late roll on a re-run: the stored legs carry the state.
    const LegRun late_rerun = run_on(all, {}, "2026-04-29", "OLD");
    ASSERT_EQ(late_rerun.rolls.size(), 1u);
    EXPECT_NEAR(late_rerun.pnl, 770.00, 1e-9);
}

// No recorded contract (no stored row, or a row written before migration 016): the roll the T-1 bar
// confirms, as before. A recorded contract that is at no consumed bar cannot be placed.
TEST(LiveRollLegs, WithoutARecordedContractTheT1BarAloneAndAnUnknownContractIsUnplaced) {
    const auto all = zm_bars();
    EXPECT_EQ(run_on(all, {}, "2026-04-28", "").rolls.size(), 1u);
    EXPECT_TRUE(run_on(all, {}, "2026-04-29", "").rolls.empty());
    const LegRun unknown = run_on(all, {}, "2026-04-29", "NEVER_HELD");
    EXPECT_TRUE(unknown.unplaced);
    EXPECT_TRUE(unknown.rolls.empty());
}

// Two rolls owed to one run (L-09): both are legged, in date order, and the unbooked moves exclude
// the second change bar (section 6.6).
TEST(LiveRollLegs, EveryRollConfirmedSinceTheRecordedContractIsLegged) {
    const std::string s = "ZC.v.0";
    const std::vector<Bar> all = {bar(s, "2026-04-20", 400.0, "H"), bar(s, "2026-04-21", 401.0, "H"),
                                  bar(s, "2026-04-22", 410.0, "K"), bar(s, "2026-04-23", 412.0, "K"),
                                  bar(s, "2026-04-24", 420.0, "N"), bar(s, "2026-04-27", 423.0, "N")};
    const auto state = live_rolls_by_state(all, {{s, "H"}}, "2026-04-27");
    ASSERT_EQ(state.rolls.size(), 2u);
    EXPECT_EQ(state.rolls[0].confirm_date, "2026-04-23");
    EXPECT_EQ(state.rolls[0].incoming_id, "K");
    EXPECT_EQ(state.rolls[1].confirm_date, "2026-04-27");
    EXPECT_EQ(state.rolls[1].outgoing_id, "K");
    EXPECT_NEAR(state.rolls[1].closing_price, 412.0, 1e-9);
    EXPECT_NEAR(state.rolls[1].opening_price, 420.0, 1e-9);
    ASSERT_TRUE(state.late.count(s));
    // Unbooked: 04-23 (412 - 410 = 2) and 04-27 (423 - 420 = 3); the change bar 04-24 books 0.
    EXPECT_NEAR(state.late.at(s).settle_to - state.late.at(s).settle_from, 5.0, 1e-9);
    EXPECT_NEAR(state.late.at(s).settle_to, 423.0, 1e-9);
}

TEST(LiveRollLegs, TheIdDateIsTheConfirmingBarsCompactDate) {
    EXPECT_EQ(compact_date("2026-05-03"), "20260503");
}
