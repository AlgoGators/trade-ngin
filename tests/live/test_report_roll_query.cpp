// R4B fix round 1 (audit A, S5): what a futures live runner asks the report's roll view for
// (live/report_roll_query.hpp), executed: the held symbols of the book and every sleeve (a zero quantity
// left out), the contract the book holds after the settled bar per symbol (the roll tracker's held id: the
// old contract on a pending switch), every ROLL closing leg with the contract its opening leg opens and the
// date in its id, a leg with no split included; the registry's tick, 0 for an unknown symbol.
#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/live/report_roll_query.hpp"

using namespace trade_ngin;

namespace {
Position pos(const std::string& symbol, double q) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(q);
    return p;
}
ExecutionReport leg(const std::string& id, const std::string& symbol, const std::string& contract,
                    ExecutionType type = ExecutionType::ROLL) {
    ExecutionReport e;
    e.exec_id = id;
    e.symbol = symbol;
    e.instrument_id = contract;
    e.execution_type = type;
    return e;
}
}  // namespace

TEST(ReportRollQuery, TheRunnerAsksForTheHeldContractsAndEveryRoll) {
    const std::unordered_map<std::string, Position> book = {{"SI.v.0", pos("SI.v.0", 1)}, {"GC.v.0", pos("GC.v.0", 0)}};
    const std::unordered_map<std::string, std::unordered_map<std::string, Position>> sleeves = {
        {"A", {{"M2K.v.0", pos("M2K.v.0", 2)}}}, {"B", {{"ZN.v.0", pos("ZN.v.0", 0)}}}};
    ExecutionReport rc = leg("EXEC_SI.v.0_20251125_RC", "SI.v.0", "42218");
    rc.roll_bar_move = -1075.0;
    const std::unordered_map<std::string, std::vector<ExecutionReport>> execs = {
        {"A", {rc, leg("EXEC_SI.v.0_20251125_RO", "SI.v.0", "42044525"),
               leg("EXEC_M2K.v.0_20260917_RC", "M2K.v.0", "42004428"),  // the vendor's series: no split
               leg("EXEC_M2K.v.0_20260917_RO", "M2K.v.0", "42099999"),
               leg("EXEC_ZN.v.0_20251126", "ZN.v.0", "", ExecutionType::STRATEGY)}}};
    roll_series::RollTracker::Status si, m2k;
    si.held_id = "42044525";
    m2k.held_id = "42004428";  // the old contract, its roll still pending
    const std::unordered_map<std::string, roll_series::RollTracker::Status> status = {
        {"SI.v.0", si}, {"M2K.v.0", m2k}, {"CL.v.0", si}};
    const auto q = report_roll_query("2025-11-25", book, sleeves, execs, status);
    EXPECT_EQ(q.date, "2025-11-25");
    EXPECT_EQ(q.held_symbols, (std::vector<std::string>{"M2K.v.0", "SI.v.0"})) << "held only, sorted";
    ASSERT_EQ(q.held_ids.size(), 2u) << "only the held symbols";
    EXPECT_EQ(q.held_ids.at("SI.v.0"), "42044525");
    EXPECT_EQ(q.held_ids.at("M2K.v.0"), "42004428");
    ASSERT_EQ(q.rolls.size(), 2u);
    const auto& r0 = q.rolls[0].symbol == "SI.v.0" ? q.rolls[0] : q.rolls[1];
    const auto& r1 = q.rolls[0].symbol == "SI.v.0" ? q.rolls[1] : q.rolls[0];
    EXPECT_EQ(r0.closing_exec_id, "EXEC_SI.v.0_20251125_RC");
    EXPECT_EQ(r0.outgoing_id, "42218");
    EXPECT_EQ(r0.incoming_id, "42044525");
    EXPECT_EQ(r0.date, "2025-11-25");
    EXPECT_EQ(r1.closing_exec_id, "EXEC_M2K.v.0_20260917_RC") << "a leg with no split is asked for too";
    EXPECT_EQ(r1.incoming_id, "42099999");
    EXPECT_EQ(r1.date, "2026-09-17");
    // An id with no date in it falls back to the settled day; no opening leg leaves the contract empty.
    const auto odd = report_roll_query("2025-11-25", book, sleeves,
                                       {{"A", {leg("ROLL_X_RC", "SI.v.0", "1")}}}, status);
    ASSERT_EQ(odd.rolls.size(), 1u);
    EXPECT_EQ(odd.rolls[0].date, "2025-11-25");
    EXPECT_EQ(odd.rolls[0].incoming_id, "");
    EXPECT_EQ(report_tick_size("NOPE.v.0"), 0.0);
}
