// Rolls step 4 (HD 2026-10-23, ruling 18; migration 040): the split of every per-contract roll, on its
// closing leg only.
//   * The tracker names the held (old) contract's move booked on the roll bar, in points: on the roll's
//     own bar for a roll known on its bar or booked after a withheld roll bar, on the switch bar for a
//     vendor switch the next bar confirms (both switch-bar settings), measured from the close the bar's
//     own move is measured from; nothing on a roll of the vendor's series (no two closes on one bar).
//   * attach_roll_split puts move = q x points x multiplier, gap = opening price - closing price and
//     gap value = gap x q x multiplier on the closing leg, signed with the position; the opening leg
//     keeps none. The worked example (silver, December to March, 2025-11-25): -1,075, 0.670, 3,350.
//   * The run's totals sum the rows that carry a split, so a roll counts once whatever the sleeves.
//   * The live state carries the points to the runner; the results row names the totals only when
//     they have a value.
#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/backtest/backtest_statistics_columns.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/roll_series.hpp"
#include "trade_ngin/data/roll_split_sql.hpp"
#include "trade_ngin/data/per_contract_source.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/live_roll_legs.hpp"
#include "trade_ngin/live/session_book_gate.hpp"

using namespace trade_ngin;
using namespace trade_ngin::roll_series;

namespace {

std::optional<RollFrom> from(double close, const char* id, RollFromKind kind = RollFromKind::KNOWN,
                             const char* date = "") {
    return RollFrom{close, id, kind, date};
}

RollLegCost flat_cost(const std::string&, double, double) {
    return RollLegCost{2.51, 0.004, 20.11, 22.62};
}

std::vector<ExecutionReport> legs_of(double q, double closing, double opening) {
    return make_roll_legs("SI.v.0", q, closing, opening, "SIZ5", "SIH6", Timestamp{}, "RC", "RC", "RO",
                          "RO", flat_cost);
}

Timestamp at(const std::string& ymd) {
    std::tm tm{};
    tm.tm_year = std::stoi(ymd.substr(0, 4)) - 1900;
    tm.tm_mon = std::stoi(ymd.substr(5, 2)) - 1;
    tm.tm_mday = std::stoi(ymd.substr(8, 2));
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Bar bar(const std::string& ymd, double close, const std::string& id,
        const std::optional<RollFrom>& rf = std::nullopt) {
    Bar b;
    b.symbol = "SI.v.0";
    b.timestamp = at(ymd);
    b.open = Decimal(close);
    b.high = Decimal(close);
    b.low = Decimal(close);
    b.close = Decimal(close);
    b.volume = 1000.0;
    b.instrument_id = id;
    b.roll_from = rf;
    return b;
}

}  // namespace

// The worked example: SIZ5 held, 11-21 49.660, 11-24 51.175; on 11-25 March crosses December's volume
// and the source hands out SIH6's bar (51.630) with December's close (50.960) in the field.
TEST(RollSplit, TheWorkedExamplesSilverRollSplitsIntoTheBookedMoveTheGapAndItsValue) {
    RollTracker t;
    EXPECT_FALSE(t.add("SIZ5", 49.660).roll_bar_points.has_value());
    EXPECT_FALSE(t.add("SIZ5", 51.175).roll_bar_points.has_value()) << "an ordinary bar carries none";
    const auto st = t.add("SIH6", 51.630, from(50.960, "SIZ5", RollFromKind::KNOWN, "2025-11-25"));
    ASSERT_TRUE(st.confirm);
    ASSERT_TRUE(st.roll_bar_points.has_value());
    EXPECT_DOUBLE_EQ(*st.roll_bar_points, 50.960 - 51.175) << "December's own move on the roll bar";
    EXPECT_DOUBLE_EQ(*st.roll_bar_points, st.move(51.630, 51.175)) << "exactly the move the bar books";
    auto legs = legs_of(1.0, st.last_close_before_change, st.change_bar_close);
    attach_roll_split(legs, 1.0, *st.roll_bar_points, 5000.0);
    ASSERT_EQ(legs.size(), 2u);
    EXPECT_NEAR(*legs[0].roll_bar_move, -1075.0, 1e-9);
    EXPECT_NEAR(*legs[0].roll_gap_points, 0.670, 1e-12);
    EXPECT_NEAR(*legs[0].roll_gap_value, 3350.0, 1e-9);
    EXPECT_EQ(legs[0].instrument_id, "SIZ5") << "the closing leg carries the split";
    EXPECT_FALSE(legs[1].roll_bar_move.has_value()) << "the opening leg carries none";
    EXPECT_FALSE(legs[1].roll_gap_points.has_value());
    EXPECT_FALSE(legs[1].roll_gap_value.has_value());
    // The next bar is March's own move from 51.630, no split.
    EXPECT_FALSE(t.add("SIH6", 53.860).roll_bar_points.has_value());
}

// A short pays nothing on the gap and books the opposite move: every figure is signed with the
// position, as the P&L is.
TEST(RollSplit, AShortsSplitIsSignedWithThePosition) {
    auto legs = legs_of(-2.0, 50.960, 51.630);
    attach_roll_split(legs, -2.0, 50.960 - 51.175, 5000.0);
    EXPECT_NEAR(*legs[0].roll_bar_move, 2150.0, 1e-9) << "short 2: the fall is a gain";
    EXPECT_NEAR(*legs[0].roll_gap_points, 0.670, 1e-12) << "points are the contracts', not the position's";
    EXPECT_NEAR(*legs[0].roll_gap_value, -6700.0, 1e-9);
    EXPECT_FALSE(legs[1].roll_bar_move.has_value());
    std::vector<ExecutionReport> none;
    attach_roll_split(none, 1.0, 1.0, 1.0);
    EXPECT_TRUE(none.empty());
}

// A vendor switch the next bar confirms: the roll is on the switch bar, so its split is the switch
// bar's booked move (A 102 - 101) and its two closes (B 110 - A 102), carried on the confirming bar.
// The "vendor" switch-bar setting books the same figures.
TEST(RollSplit, AConfirmedVendorSwitchSplitsOnItsSwitchBarUnderBothSettings) {
    RollTracker held;
    held.add("A", 100);
    held.add("A", 101);
    const auto sw = held.add("A", 102, from(110, "B", RollFromKind::AWAITS_NEXT, "d2"));
    EXPECT_FALSE(sw.roll_bar_points.has_value()) << "not known on the switch bar";
    const auto c = held.add("B", 111, from(104, "A", RollFromKind::AFTER_ROLL, "d2"));
    ASSERT_TRUE(c.confirm);
    ASSERT_TRUE(c.roll_bar_points.has_value());
    EXPECT_DOUBLE_EQ(*c.roll_bar_points, 1.0) << "A's move on the switch bar, not the confirming bar's";
    EXPECT_DOUBLE_EQ(c.change_bar_close - c.last_close_before_change, 8.0);

    RollTracker vendor;
    vendor.add("A", 100);
    vendor.add("A", 101);
    EXPECT_FALSE(vendor.add("B", 110, from(102, "A", RollFromKind::AWAITS_NEXT_VENDOR, "d2"))
                     .roll_bar_points.has_value());
    const auto cv = vendor.add("B", 111, from(104, "A", RollFromKind::AFTER_ROLL, "d2"));
    ASSERT_TRUE(cv.confirm);
    ASSERT_TRUE(cv.roll_bar_points.has_value());
    EXPECT_DOUBLE_EQ(*cv.roll_bar_points, *c.roll_bar_points);
    EXPECT_DOUBLE_EQ(cv.change_bar_close - cv.last_close_before_change, 8.0);

    // Not confirmed: nothing rolled, no split anywhere.
    RollTracker stray;
    stray.add("A", 100);
    stray.add("A", 101);
    stray.add("A", 102, from(110, "B", RollFromKind::AWAITS_NEXT, "d2"));
    const auto back = stray.add("A", 103);
    EXPECT_FALSE(back.confirm);
    EXPECT_FALSE(back.roll_bar_points.has_value());
}

// A consumer that withheld the roll bar books the roll on its next consumed bar: the old contract's
// move from its last consumed close (101) to its close on that bar (103).
TEST(RollSplit, AWithheldRollBarSplitsOnTheBarThatBooksIt) {
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101);
    const auto st = t.add("B", 111, from(103, "A", RollFromKind::AFTER_ROLL, "d2"));
    ASSERT_TRUE(st.confirm);
    ASSERT_TRUE(st.roll_bar_points.has_value());
    EXPECT_DOUBLE_EQ(*st.roll_bar_points, 2.0);
    EXPECT_DOUBLE_EQ(st.change_bar_close - st.last_close_before_change, 8.0);
}

// "vendor": a switch bar the next bar does not confirm moves the next bar from the held contract's
// close on the switch bar (102); a roll known on that next bar is split from it.
TEST(RollSplit, ARollAfterAnUnconfirmedVendorSwitchBarIsSplitFromTheHeldClose) {
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101);
    t.add("B", 110, from(102, "A", RollFromKind::AWAITS_NEXT_VENDOR, "d2"));
    const auto st = t.add("C", 120, from(105, "A", RollFromKind::KNOWN, "d3"));
    ASSERT_TRUE(st.confirm);
    ASSERT_TRUE(st.roll_bar_points.has_value());
    EXPECT_DOUBLE_EQ(*st.roll_bar_points, 3.0) << "105 - 102, the move the bar books";
    EXPECT_DOUBLE_EQ(*st.roll_bar_points, st.move(120, 110));
}

// The vendor's series (the switch off, the micros): the switch bar books 0 and the gap is not known.
TEST(RollSplit, ARollOfTheVendorsSeriesHasNoSplit) {
    RollTracker t;
    t.add("A", 100);
    t.add("A", 101);
    EXPECT_FALSE(t.add("B", 110).roll_bar_points.has_value());
    const auto c = t.add("B", 111);
    ASSERT_TRUE(c.confirm);
    EXPECT_FALSE(c.roll_bar_points.has_value());
}

// Two sleeves holding the symbol each carry their own split on their own closing leg; the totals sum
// the rows that carry one, so the roll counts once per sleeve and the opening legs and fills add 0.
TEST(RollSplit, TheRunsTotalsSumEachRollOnce) {
    auto a = legs_of(1.0, 50.960, 51.630);
    attach_roll_split(a, 1.0, -0.215, 5000.0);
    auto b = legs_of(3.0, 50.960, 51.630);
    attach_roll_split(b, 3.0, -0.215, 5000.0);
    std::vector<ExecutionReport> rows = a;
    rows.insert(rows.end(), b.begin(), b.end());
    ExecutionReport fill;
    fill.symbol = "ZN.v.0";
    rows.push_back(fill);
    const auto totals = roll_split_totals(rows);
    EXPECT_EQ(totals.rolls, 2);
    EXPECT_NEAR(totals.roll_bar_moves, -1075.0 * 4, 1e-6);
    EXPECT_NEAR(totals.roll_gap_values, 3350.0 * 4, 1e-6);
    const auto empty = roll_split_totals({fill});
    EXPECT_EQ(empty.rolls, 0);
    EXPECT_EQ(empty.roll_bar_moves, 0.0);
    EXPECT_EQ(empty.roll_gap_values, 0.0);
}

// The live state hands the runner the tracker's points for each roll it legs, and none for a roll of
// the vendor's series.
TEST(RollSplit, TheLiveStateCarriesThePointsToTheRunner) {
    const std::vector<Bar> consumed = {bar("2025-11-21", 49.660, "SIZ5"), bar("2025-11-24", 51.175, "SIZ5"),
                                       bar("2025-11-25", 51.630, "SIH6",
                                           from(50.960, "SIZ5", RollFromKind::KNOWN, "2025-11-25"))};
    const auto state = live_rolls_by_state(consumed, {{"SI.v.0", "SIZ5"}}, "2025-11-25");
    ASSERT_EQ(state.rolls.size(), 1u);
    ASSERT_TRUE(state.rolls[0].roll_bar_points.has_value());
    EXPECT_DOUBLE_EQ(*state.rolls[0].roll_bar_points, 50.960 - 51.175);
    EXPECT_TRUE(state.rolls[0].known_on_bar);

    const std::vector<Bar> vendor = {bar("2025-11-21", 49.660, "SIZ5"), bar("2025-11-24", 51.840, "SIH6"),
                                     bar("2025-11-25", 51.630, "SIH6")};
    const auto vs = live_rolls_by_state(vendor, {{"SI.v.0", "SIZ5"}}, "2025-11-25");
    ASSERT_EQ(vs.rolls.size(), 1u);
    EXPECT_FALSE(vs.rolls[0].roll_bar_points.has_value());
}

// The results row names the two totals only when they have a value (the switch on), at round-trip
// precision; with the switch off it names neither, so its statement is the one before 040.
TEST(RollSplit, TheResultsRowNamesTheTotalsOnlyWithTheSwitchOn) {
    EXPECT_TRUE(roll_split_summary_cells(std::nullopt, std::nullopt).empty());
    const auto cells = roll_split_summary_cells(-1075.0, 3350.125);
    ASSERT_EQ(cells.size(), 2u);
    EXPECT_EQ(cells[0].column, "roll_bar_moves");
    EXPECT_EQ(cells[0].type, "numeric");
    EXPECT_EQ(*cells[0].value, "-1075");
    EXPECT_EQ(cells[1].column, "roll_gap_values");
    EXPECT_EQ(*cells[1].value, "3350.125");
    const auto zero = roll_split_summary_cells(0.0, 0.0);
    ASSERT_EQ(zero.size(), 2u) << "a switch-on run with no roll stores 0, not NULL";
    EXPECT_EQ(*zero[0].value, "0");
}

// R4b (the report's roll footnote): the T-1 settlement names the symbols whose T-1 bar is a per-contract
// roll bar (the row books the old contract's move to its close on it), none on an ordinary bar, none on a
// roll of the vendor's series, and none for a symbol whose roll the run legs late.
TEST(RollSplit, TheSettlementNamesThePerContractRollBarsForTheFootnote) {
    const std::vector<Bar> consumed = {bar("2025-11-21", 49.660, "SIZ5"), bar("2025-11-24", 51.175, "SIZ5"),
                                       bar("2025-11-25", 51.630, "SIH6",
                                           from(50.960, "SIZ5", RollFromKind::KNOWN, "2025-11-25"))};
    const auto on_roll = consumed_t1_settlement(consumed, "2025-11-25", roll_status_of(consumed), {}, {}, {}, {});
    EXPECT_EQ(on_roll.per_contract_roll_symbols, (std::unordered_set<std::string>{"SI.v.0"}));
    EXPECT_TRUE(on_roll.roll_zero_symbols.empty()) << "the roll bar books a move";
    EXPECT_DOUBLE_EQ(on_roll.t1_close_prices.at("SI.v.0"), 50.960);
    const std::vector<Bar> before(consumed.begin(), consumed.begin() + 2);
    EXPECT_TRUE(consumed_t1_settlement(before, "2025-11-24", roll_status_of(before), {}, {}, {}, {})
                    .per_contract_roll_symbols.empty());
    const std::vector<Bar> vendor = {bar("2025-11-21", 49.660, "SIZ5"), bar("2025-11-24", 51.840, "SIH6")};
    const auto change = consumed_t1_settlement(vendor, "2025-11-24", roll_status_of(vendor), {}, {}, {}, {});
    EXPECT_TRUE(change.per_contract_roll_symbols.empty());
    EXPECT_EQ(change.roll_zero_symbols, (std::unordered_set<std::string>{"SI.v.0"})) << "today's change bar";
    const auto late = consumed_t1_settlement(consumed, "2025-11-25", roll_status_of(consumed), {}, {}, {},
                                             {{"SI.v.0", LateRollSettlement{51.630, 49.660, "2025-11-21"}}});
    EXPECT_TRUE(late.per_contract_roll_symbols.empty());
}

// The writers' statements (roll_split_sql.hpp): with no split in the call they name none of 040's columns
// (the statements before 040); with one, the three columns, their placeholders from the given number, and
// the three values in order (NULL where a row has none).
TEST(RollSplit, TheWritersNameTheColumnsOnlyForASplit) {
    auto legs = legs_of(1.0, 50.960, 51.630);
    attach_roll_split(legs, 1.0, -0.215, 5000.0);
    EXPECT_TRUE(roll_split_sql::has_split(legs[0]));
    EXPECT_FALSE(roll_split_sql::has_split(legs[1]));
    EXPECT_TRUE(roll_split_sql::any_split(legs));
    EXPECT_FALSE(roll_split_sql::any_split({legs[1]}));
    EXPECT_FALSE(roll_split_sql::any_split({}));
    EXPECT_EQ(roll_split_sql::columns(false), "");
    EXPECT_EQ(roll_split_sql::columns(true), ", roll_bar_move, roll_gap_points, roll_gap_value");
    EXPECT_EQ(roll_split_sql::placeholders(false, 21), "");
    EXPECT_EQ(roll_split_sql::placeholders(true, 21), ", $21, $22, $23");
    EXPECT_EQ(roll_split_sql::placeholders(true, 19), ", $19, $20, $21");
    pqxx::params p;
    roll_split_sql::append(p, legs[0]);
    roll_split_sql::append(p, legs[1]);
    EXPECT_EQ(p.size(), 6);
}

// The database's roll view with nothing to read: the switch off gives an empty view (nothing new printed);
// the switch on with no connection gives the switch-on view with no line, box or cell (the roll sentences
// only), with a WARN.
TEST(RollSplit, TheDatabasesRollViewWithNothingToRead) {
    PostgresDatabase db("postgresql://nobody@127.0.0.1:1/none");  // never connected
    contract_rolls::ReportRollQuery q;
    q.date = "2025-11-21";
    q.held_symbols = {"SI.v.0"};
    contract_rolls::PerContractSource::instance().disable();
    const auto off = db.report_roll_view(q, {});
    EXPECT_FALSE(off.per_contract_rolls);
    contract_rolls::PerContractSource::instance().enable(contract_rolls::FuturesCalendar{});
    const auto on = db.report_roll_view(q, {});
    contract_rolls::PerContractSource::instance().disable();
    EXPECT_TRUE(on.per_contract_rolls);
    EXPECT_TRUE(on.outlook.empty());
    EXPECT_TRUE(on.names.empty());
}

// S4 (R4B audit A): the results row's cells are the statistics' cells then the two totals, only with a value.
TEST(RollSplit, TheResultsRowStoresTheStatisticsCellsThenTheTotals) {
    BacktestStatisticsColumns columns;
    const auto stats = backtest_statistics_cells(columns);
    const auto off = backtest_summary_cells(columns, std::nullopt, std::nullopt);
    ASSERT_EQ(off.size(), stats.size()) << "switch off: the statement before 040";
    const auto on = backtest_summary_cells(columns, -43678.1375, -38805.5);
    ASSERT_EQ(on.size(), stats.size() + 2);
    EXPECT_EQ(on[on.size() - 2].column, "roll_bar_moves");
    EXPECT_EQ(*on[on.size() - 2].value, "-43678.1375");
    EXPECT_EQ(on.back().column, "roll_gap_values");
    EXPECT_EQ(*on.back().value, "-38805.5");
    for (size_t i = 0; i < stats.size(); ++i) EXPECT_EQ(on[i].column, stats[i].column);
}

// S7 m13 (R4B audit A): a "vendor" switch bar (per_contract_switch_bar vendor: the target's own bar, the
// field naming the held contract) carries the held contract's close as its mark but is no roll: nothing
// rolled yet (or ever, for a stray), so the settled row gets no "Contract rolled that day" footnote.
TEST(RollSplit, AVendorSettingSwitchBarIsNoRollBarForTheFootnote) {
    const std::vector<Bar> consumed = {bar("2025-11-21", 49.660, "SIZ5"), bar("2025-11-24", 51.175, "SIZ5"),
                                       bar("2025-11-25", 51.630, "SIH6",
                                           from(50.960, "SIZ5", RollFromKind::AWAITS_NEXT_VENDOR, "2025-11-25"))};
    const auto status = roll_status_of(consumed);
    ASSERT_TRUE(status.at("SI.v.0").mark_close.has_value());
    ASSERT_FALSE(status.at("SI.v.0").confirm);
    const auto st = consumed_t1_settlement(consumed, "2025-11-25", status, {}, {}, {}, {});
    EXPECT_TRUE(st.per_contract_roll_symbols.empty());
    EXPECT_DOUBLE_EQ(st.t1_close_prices.at("SI.v.0"), 50.960) << "the held contract's close";
}
