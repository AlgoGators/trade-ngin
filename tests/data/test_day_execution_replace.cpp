// tests/data/test_day_execution_replace.cpp
//
// T-DATED R3 (HD 2026-10-10). The sweep of a sleeve's executions of a day and the store of the
// run's rows are ONE transaction. These tests EXECUTE replace_day_executions_in, the function
// PostgresDatabase::replace_roll_day_executions runs on its pqxx transaction, against an in-memory
// stand-in for the transaction: no database is needed, and the stand-in's store can be made to
// fail. The same cases on a real database are in tests/data/test_roll_state_db.cpp.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/data/day_execution_replace.hpp"

using namespace trade_ngin;

namespace {

struct Row {
    std::string day, sleeve, portfolio, order_id;
    bool operator==(const Row& o) const {
        return day == o.day && sleeve == o.sleeve && portfolio == o.portfolio &&
               order_id == o.order_id;
    }
};

// A transaction over a table of rows: every step works on a pending copy, commit() makes it the
// table, and a stand-in that is never committed leaves the table as it was (a rollback).
struct StandInTxn {
    std::vector<Row>& table;
    std::vector<Row> pending;
    std::string day;
    std::string portfolio;
    std::string fail_insert_for;  ///< the sleeve whose insert fails; empty: none
    std::vector<std::string> steps;
    bool committed{false};

    StandInTxn(std::vector<Row>& t, std::string d, std::string p)
        : table(t), pending(t), day(std::move(d)), portfolio(std::move(p)) {}

    void delete_day(const std::string& sleeve, const std::string& pf, const std::string& d) {
        steps.push_back("sweep " + sleeve);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Row& r) {
                                         return r.sleeve == sleeve && r.portfolio == pf && r.day == d;
                                     }),
                      pending.end());
    }
    void delete_order_ids(const std::string& sleeve, const std::string& pf,
                          const std::vector<std::string>& ids) {
        steps.push_back("ids " + sleeve);
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [&](const Row& r) {
                                         return r.sleeve == sleeve && r.portfolio == pf &&
                                                std::find(ids.begin(), ids.end(), r.order_id) !=
                                                    ids.end();
                                     }),
                      pending.end());
    }
    Result<void> insert(const std::string& sleeve, const std::vector<ExecutionReport>& executions) {
        steps.push_back("insert " + sleeve);
        if (sleeve == fail_insert_for) {
            return make_error<void>(ErrorCode::DATABASE_ERROR, "the store failed", "StandInTxn");
        }
        for (const auto& e : executions) pending.push_back({day, sleeve, portfolio, e.order_id});
        return Result<void>();
    }
    void commit() {
        steps.push_back("commit");
        table = pending;
        committed = true;
    }
};

ExecutionReport order(const std::string& id) {
    ExecutionReport e;
    e.exec_id = id;
    e.order_id = id;
    return e;
}

const std::string kDay = "2026-05-01";
const std::string kBook = "CONSERVATIVE_PORTFOLIO";

std::vector<Row> first_run() {
    return {{"2026-04-30", "TREND_FOLLOWING", kBook, "EXEC_MBT.v.0_20260430"},
            {kDay, "TREND_FOLLOWING", kBook, "EXEC_MBT.v.0_20260501_SC"},  // the re-run drops it
            {kDay, "TREND_FOLLOWING", kBook, "EXEC_6A.v.0_20260501"},
            {kDay, "TREND_FOLLOWING_FAST", kBook, "EXEC_ZF.v.0_20260501"},
            {kDay, "TREND_FOLLOWING", "BASE_PORTFOLIO", "EXEC_MBT.v.0_20260501_SC"}};
}

}  // namespace

TEST(DayExecutionReplace, TheSweepTheOrderIdDeleteAndTheInsertRunInThatOrderThenOneCommit) {
    std::vector<Row> table = first_run();
    StandInTxn txn(table, kDay, kBook);
    const std::vector<ExecutionReport> rerun = {order("EXEC_6A.v.0_20260501"),
                                                order("EXEC_6A.v.0_20260501"),
                                                order("EXEC_ZC.v.0_20260501")};
    const auto replaced = replace_day_executions_in(
        txn, {{"TREND_FOLLOWING", rerun}, {"TREND_FOLLOWING_FAST", {}}}, kBook, kDay);
    ASSERT_TRUE(replaced.is_ok());
    const std::vector<std::string> want_steps = {
        "sweep TREND_FOLLOWING",      "ids TREND_FOLLOWING", "insert TREND_FOLLOWING",
        "sweep TREND_FOLLOWING_FAST", "insert TREND_FOLLOWING_FAST", "commit"};
    EXPECT_EQ(txn.steps, want_steps) << "a sleeve with no order is swept and has no id delete";

    // The order the re-run no longer makes is gone; the sleeve the run makes no order for is
    // emptied; another day and another portfolio are untouched.
    const std::vector<Row> want = {
        {"2026-04-30", "TREND_FOLLOWING", kBook, "EXEC_MBT.v.0_20260430"},
        {kDay, "TREND_FOLLOWING", "BASE_PORTFOLIO", "EXEC_MBT.v.0_20260501_SC"},
        {kDay, "TREND_FOLLOWING", kBook, "EXEC_6A.v.0_20260501"},
        {kDay, "TREND_FOLLOWING", kBook, "EXEC_6A.v.0_20260501"},
        {kDay, "TREND_FOLLOWING", kBook, "EXEC_ZC.v.0_20260501"}};
    EXPECT_EQ(table, want);
}

TEST(DayExecutionReplace, AStoreThatFailsCommitsNothingSoTheSleevesDayIsNotLost) {
    // The defect (audit A F2): the sweep committed in its own transaction, and a store that then
    // failed lost every execution of the sleeve's day.
    std::vector<Row> table = first_run();
    const std::vector<Row> before = table;
    StandInTxn txn(table, kDay, kBook);
    txn.fail_insert_for = "TREND_FOLLOWING";
    const auto replaced = replace_day_executions_in(
        txn, {{"TREND_FOLLOWING", {order("EXEC_6A.v.0_20260501")}}}, kBook, kDay);
    ASSERT_TRUE(replaced.is_error());
    EXPECT_EQ(std::string(replaced.error()->what()), "the store failed");
    EXPECT_FALSE(txn.committed);
    EXPECT_EQ(txn.steps.back(), "insert TREND_FOLLOWING") << "nothing runs after the failure";
    EXPECT_EQ(table, before) << "the sweep was committed apart from the store";
    EXPECT_NE(txn.pending, before) << "the sweep did run, inside the transaction";
}

TEST(DayExecutionReplace, AFailureOnALaterSleeveCommitsNoEarlierSleeveEither) {
    std::vector<Row> table = first_run();
    const std::vector<Row> before = table;
    StandInTxn txn(table, kDay, kBook);
    txn.fail_insert_for = "TREND_FOLLOWING_FAST";
    const auto replaced = replace_day_executions_in(
        txn,
        {{"TREND_FOLLOWING", {order("EXEC_6A.v.0_20260501")}},
         {"TREND_FOLLOWING_FAST", {order("EXEC_ZF.v.0_20260501")}}},
        kBook, kDay);
    ASSERT_TRUE(replaced.is_error());
    EXPECT_FALSE(txn.committed);
    EXPECT_EQ(table, before);
}

TEST(DayExecutionReplace, AFirstRunOfADateRemovesNothing) {
    std::vector<Row> table = {{"2026-04-30", "TREND_FOLLOWING", kBook, "EXEC_MBT.v.0_20260430"}};
    StandInTxn txn(table, kDay, kBook);
    ASSERT_TRUE(replace_day_executions_in(
                    txn, {{"TREND_FOLLOWING", {order("EXEC_6A.v.0_20260501")}}}, kBook, kDay)
                    .is_ok());
    const std::vector<Row> want = {{"2026-04-30", "TREND_FOLLOWING", kBook, "EXEC_MBT.v.0_20260430"},
                                   {kDay, "TREND_FOLLOWING", kBook, "EXEC_6A.v.0_20260501"}};
    EXPECT_EQ(table, want);
}
