// QT hardening (2026-10-09), the engine's reads and writes of the desk's command log and books
// against a real server (src/live/qt_desk.cpp):
//   C1  load_proposal_rows + proposal_sha256 reproduce the snapshot hash AlgoLens writes; an
//       approval is refused when the proposal changed, or when the request has no snapshot;
//   C2  one decision per request; the request must be done;
//   C4  finish_audit_row only finishes a pending or running row, and merges its result;
//   2   clear_desk_day deletes the qt day's non-ROLL executions (never ROLL, never another book)
//       and clears the completion marker;
//   3   publish_blocker refuses an unmarked qt day and a day whose last desk command failed;
//   7   mark_email_sent / publish_email_sent record and find a publish e-mail.
//
// The database needs the trading schema with migrations 021 and 023 (trading.position_overrides
// and live_results.book_source), e.g. a throwaway restore. position_overrides rows are never
// deleted (migration 023), so every run uses its own portfolio id; the book rows are removed.
// Reachability gate as tests/data/test_qt_books_db.cpp: TRADE_NGIN_TEST_DSN, and
// TRADE_NGIN_REQUIRE_DB=1 to fail rather than skip.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/live/qt_desk.hpp"

using namespace trade_ngin;

namespace {

constexpr const char* kSid = "LIVE_QTDESK_PROBE";
constexpr const char* kSleeve = "TREND_FOLLOWING";
constexpr const char* kDate = "2026-10-06";

std::string dsn() {
    const char* d = std::getenv("TRADE_NGIN_TEST_DSN");
    return (d && *d) ? std::string(d) : std::string();
}

Timestamp day_at(int h = 21) {
    std::tm tm{};
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 6;
    tm.tm_hour = h;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

ExecutionReport fill(const std::string& symbol, ExecutionType type, const std::string& suffix) {
    ExecutionReport e;
    e.symbol = symbol;
    e.order_id = "qt-DAILY_" + symbol + "_20261006" + suffix;
    e.exec_id = "EXEC_" + symbol + "_20261006" + suffix;
    e.side = Side::BUY;
    e.filled_quantity = Quantity(2.0);
    e.fill_price = Price(100.0);
    e.fill_time = day_at();
    e.commissions_fees = Decimal(1.0);
    e.implicit_price_impact = Decimal(0.0);
    e.slippage_market_impact = Decimal(0.0);
    e.total_transaction_costs = Decimal(1.0);
    e.is_partial = false;
    e.execution_type = type;
    if (type == ExecutionType::ROLL) e.instrument_id = "C" + suffix;
    return e;
}

}  // namespace

class QtDeskDb : public ::testing::Test {
protected:
    void SetUp() override {
        const char* r = std::getenv("TRADE_NGIN_REQUIRE_DB");
        const bool require = r && std::string(r) == "1";
        conn_ = dsn();
        if (conn_.empty()) {
            if (require) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set";
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn_);
        if (db_->connect().is_error() || !db_->is_connected()) {
            if (require) FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            GTEST_SKIP() << "database unreachable";
        }
        if (scalar("SELECT (to_regclass('trading.position_overrides') IS NOT NULL AND EXISTS "
                   "(SELECT 1 FROM information_schema.columns WHERE table_schema = 'trading' "
                   "AND table_name = 'live_results' AND column_name = 'book_source'))::text") !=
            "true") {
            FAIL() << "the test database lacks migration 023 (trading.position_overrides)";
        }
        pid_ = "QTDESK_PROBE_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000);
        purge();
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) {
            purge();
            db_->disconnect();
        }
    }

    void purge() {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        for (const char* t : {"trading.positions", "trading.executions", "trading.live_results"}) {
            w.exec(std::string("DELETE FROM ") + t + " WHERE portfolio_id = $1",
                   pqxx::params{pid_});
        }
        w.commit();
    }

    std::string scalar(const std::string& sql) {
        pqxx::connection c(conn_);
        pqxx::work w(c);
        auto r = w.exec(sql);
        w.commit();
        return (r.empty() || r[0][0].is_null()) ? std::string() : r[0][0].as<std::string>();
    }

    std::string lit(const std::string& s) { return qt::sql_literal(s); }

    // A command row as AlgoLens inserts it (pending), then moved as the agent and engine move it.
    long command(const std::string& kind, const std::string& payload = "{}", long parent = 0,
                 const std::string& by = "desk@x.org") {
        const std::string id = scalar(
            "INSERT INTO trading.position_overrides (portfolio_id, date, kind, requested_by, "
            "reason, payload, parent_id, approver_role) VALUES (" + lit(pid_) + ", " +
            lit(kDate) + ", " + lit(kind) + ", " + lit(by) + ", 'probe', " + lit(payload) +
            "::jsonb, " + (parent > 0 ? std::to_string(parent) : std::string("NULL")) + ", " +
            (kind == "override_decision" ? std::string("'vp'") : std::string("NULL")) +
            ") RETURNING id");
        return std::stol(id);
    }
    void move(long id, const std::string& status) {
        scalar("UPDATE trading.position_overrides SET status = " + lit(status) +
               " WHERE id = " + std::to_string(id) + " RETURNING id");
    }
    // A request that was e-mailed: running, token, done.
    long emailed_request(const std::string& payload) {
        const long id = command("override_request", payload);
        move(id, "running");
        scalar("UPDATE trading.position_overrides SET token_hash = md5(random()::text), "
               "token_expires_at = now() + interval '48 hours' WHERE id = " +
               std::to_string(id) + " RETURNING id");
        move(id, "done");
        return id;
    }

    void proposal_row(const std::string& symbol, double quantity, const std::string& book = "qt_proposal") {
        scalar("INSERT INTO trading.positions (symbol, quantity, average_price, "
               "daily_unrealized_pnl, daily_realized_pnl, last_update, strategy_id, strategy_name, "
               "portfolio_id, date, portfolio_type) VALUES (" +
               lit(symbol) + ", " + std::to_string(quantity) +
               ", 1.0, 0, 0, '2026-10-06 00:00:00+00', " +
               lit(kSid) + ", " + lit(kSleeve) + ", " + lit(pid_) + ", " + lit(kDate) + ", " +
               lit(book) + ") RETURNING symbol");
    }

    void seed_book() {
        // inserted out of order, plus a system row the snapshot never reads
        proposal_row("ZN.v.0", 15);
        proposal_row("6A.v.0", -2);
        proposal_row("MES.v.0", 0);
        proposal_row("6A.v.0", -9, "system");
    }

    qt::AuditRow row(long id) {
        auto r = qt::load_audit_row(*db_, id);
        EXPECT_TRUE(r.is_ok());
        return r.value();
    }

    std::string conn_;
    std::string pid_;
    std::shared_ptr<PostgresDatabase> db_;
};

// C1: the engine reads the same rows AlgoLens snapshots and hashes them to Python's bytes.
TEST_F(QtDeskDb, TheProposalRowsHashToPythonsSnapshot) {
    seed_book();
    auto rows = qt::load_proposal_rows(*db_, pid_, kDate);
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    ASSERT_EQ(rows.value().size(), 3u);
    EXPECT_EQ(rows.value()[0].symbol, "6A.v.0");
    EXPECT_EQ(rows.value()[2].symbol, "ZN.v.0");
    EXPECT_EQ(qt::proposal_sha256(rows.value()),
              "11608a8e156036f756d6a99ce817e1d28daeaf19659efd66c9896d1be45c2979");
    proposal_row("CL.v.0", 1.5);
    EXPECT_TRUE(qt::load_proposal_rows(*db_, pid_, kDate).is_error())
        << "a fractional quantity has no integer snapshot";
}

// C1 + C2 through check_override_decision.
TEST_F(QtDeskDb, AnApprovalNeedsTheUnchangedSnapshotAndOneDecision) {
    seed_book();
    auto rows = qt::load_proposal_rows(*db_, pid_, kDate);
    ASSERT_TRUE(rows.is_ok());
    const std::string h = qt::proposal_sha256(rows.value());
    const nlohmann::json snapshot = {
        {"proposal", nlohmann::json::array({{{"strategy_name", kSleeve}, {"symbol", "6A.v.0"}, {"quantity", -2}},
                                            {{"strategy_name", kSleeve}, {"symbol", "MES.v.0"}, {"quantity", 0}},
                                            {{"strategy_name", kSleeve}, {"symbol", "ZN.v.0"}, {"quantity", 15}}})},
        {"proposal_sha256", h}};

    // no snapshot: refused
    const long old_request = emailed_request("{}");
    const long old_decision = command("override_decision", R"({"approved":true})", old_request, "vp@x.org");
    auto refused = qt::check_override_decision(*db_, row(old_decision));
    ASSERT_TRUE(refused.is_error());
    EXPECT_NE(std::string(refused.error()->what()).find("no proposal snapshot"), std::string::npos)
        << refused.error()->what();
    ASSERT_TRUE(qt::finish_audit_row(*db_, old_decision, "refused", nlohmann::json::object(),
                                     refused.error()->what())
                    .is_ok());
    // ... and so is a rejection of it: the desk requests again
    const long reject = command("override_decision", R"({"approved":false})", old_request, "vp@x.org");
    EXPECT_TRUE(qt::check_override_decision(*db_, row(reject)).is_error());
    ASSERT_TRUE(qt::finish_audit_row(*db_, reject, "refused", nlohmann::json::object(), "x").is_ok());

    // the request is not done yet: refused
    const long pending_request = command("override_request", snapshot.dump());
    const long early = command("override_decision", R"({"approved":true})", pending_request, "vp@x.org");
    auto not_done = qt::check_override_decision(*db_, row(early));
    ASSERT_TRUE(not_done.is_error());
    EXPECT_NE(std::string(not_done.error()->what()).find("not done"), std::string::npos);
    // (migration 025 keeps one open request per day: this one fails, its decision is refused)
    ASSERT_TRUE(qt::finish_audit_row(*db_, early, "refused", nlohmann::json::object(), "x").is_ok());
    ASSERT_TRUE(qt::finish_audit_row(*db_, pending_request, "failed", nlohmann::json::object(), "x")
                    .is_ok());

    // the snapshot matches: approved
    const long request = emailed_request(snapshot.dump());
    const long decision = command("override_decision", R"({"approved":true})", request, "vp@x.org");
    auto approved = qt::check_override_decision(*db_, row(decision));
    ASSERT_TRUE(approved.is_ok()) << approved.error()->what();
    EXPECT_TRUE(approved.value());

    // the proposal moved after the request: refused with the contract's message
    scalar("UPDATE trading.positions SET quantity = 16 WHERE portfolio_id = " + lit(pid_) +
           " AND symbol = 'ZN.v.0' AND portfolio_type = 'qt_proposal' RETURNING symbol");
    auto changed = qt::check_override_decision(*db_, row(decision));
    ASSERT_TRUE(changed.is_error());
    EXPECT_EQ(std::string(changed.error()->what()).find(qt::kProposalChanged) != std::string::npos, true)
        << changed.error()->what();
    scalar("UPDATE trading.positions SET quantity = 15 WHERE portfolio_id = " + lit(pid_) +
           " AND symbol = 'ZN.v.0' AND portfolio_type = 'qt_proposal' RETURNING symbol");

    // C2: once one decision is done, another is refused. With migration 025 its unique index
    // refuses the second row's insert already; without it the engine refuses the decision.
    move(decision, "running");
    ASSERT_TRUE(qt::finish_audit_row(*db_, decision, "done", {{"approved", true}}, "booked").is_ok());
    long second = 0;
    try {
        second = command("override_decision", R"({"approved":true})", request, "pres@x.org");
    } catch (const pqxx::unique_violation&) {
        SUCCEED() << "migration 025's one-decision index refused the second decision";
        return;
    }
    auto twice = qt::check_override_decision(*db_, row(second));
    ASSERT_TRUE(twice.is_error());
    EXPECT_NE(std::string(twice.error()->what()).find("already decided"), std::string::npos)
        << twice.error()->what();
}

// C4: a terminal row is final; the result is merged, so a key written while it ran stays; a row
// refused before it was started goes pending -> running -> refused (migration 025's transitions).
TEST_F(QtDeskDb, FinishOnlyFinishesAPendingOrRunningRow) {
    const long early = command("save");
    ASSERT_TRUE(qt::finish_audit_row(*db_, early, "refused", nlohmann::json::object(), "early").is_ok());
    EXPECT_EQ(scalar("SELECT status || ' ' || (started_at IS NOT NULL)::text FROM "
                     "trading.position_overrides WHERE id = " + std::to_string(early)),
              "refused true");
    const long id = command("publish");
    move(id, "running");
    auto sent = qt::mark_email_sent(*db_, id);
    ASSERT_TRUE(sent.is_ok()) << sent.error()->what();
    ASSERT_TRUE(qt::finish_audit_row(*db_, id, "done", {{"emailed", true}}, "published").is_ok());
    EXPECT_EQ(scalar("SELECT status || ' ' || (result ? 'email_sent_at')::text || ' ' || "
                     "(result->>'emailed') FROM trading.position_overrides WHERE id = " +
                     std::to_string(id)),
              "done true true");
    auto again = qt::finish_audit_row(*db_, id, "failed", {{"x", 1}}, "late");
    EXPECT_TRUE(again.is_error()) << "a done row was finished again";
    EXPECT_EQ(scalar("SELECT status || ' ' || message FROM trading.position_overrides WHERE id = " +
                     std::to_string(id)),
              "done published");
    EXPECT_TRUE(qt::mark_email_sent(*db_, id).is_error()) << "email_sent_at on a done row";
}

// Item 7: the day's sent e-mail is found from any publish row of the day.
TEST_F(QtDeskDb, ThePublishEmailIsFoundFromAnyPublishRowOfTheDay) {
    auto none = qt::publish_email_sent(*db_, pid_, kDate);
    ASSERT_TRUE(none.is_ok()) << none.error()->what();
    EXPECT_TRUE(none.value().at.empty());
    const long first = command("publish");
    move(first, "running");
    ASSERT_TRUE(qt::mark_email_sent(*db_, first).is_ok());
    ASSERT_TRUE(qt::finish_audit_row(*db_, first, "failed", {{"emailed", true}}, "set_published failed").is_ok());
    auto found = qt::publish_email_sent(*db_, pid_, kDate);
    ASSERT_TRUE(found.is_ok());
    EXPECT_EQ(found.value().row_id, first);
    EXPECT_FALSE(found.value().at.empty());
}

// Item 2: the desk's clear takes the qt day's non-ROLL executions and the marker, nothing else.
TEST_F(QtDeskDb, ClearDeskDayTakesOnlyTheQtDaysNonRollExecutions) {
    const auto d = day_at();
    ASSERT_TRUE(db_->store_executions({fill("ZN.v.0", ExecutionType::STRATEGY, ""),
                                       fill("MES.v.0", ExecutionType::STRATEGY, "")},
                                      kSid, kSleeve, pid_, "trading.executions", "qt")
                    .is_ok());
    ASSERT_TRUE(db_->store_executions({fill("ZN.v.0", ExecutionType::ROLL, "RC")}, kSid, kSleeve,
                                      pid_, "trading.executions", "qt")
                    .is_ok());
    ASSERT_TRUE(db_->store_executions({fill("ZN.v.0", ExecutionType::STRATEGY, "")}, kSid, kSleeve,
                                      pid_, "trading.executions", "system")
                    .is_ok());
    const std::unordered_map<std::string, double> m{{"current_portfolio_value", 500000.0}};
    ASSERT_TRUE(db_->store_live_results_complete(kSid, d, m, {}, nlohmann::json(), pid_,
                                                 "trading.live_results", nlohmann::json(), "qt")
                    .is_ok());
    scalar("UPDATE trading.live_results SET book_source = 'model' WHERE portfolio_id = " + lit(pid_) +
           " RETURNING 1");

    ASSERT_TRUE(qt::clear_desk_day(*db_, pid_, kSid, kDate).is_ok());
    EXPECT_EQ(scalar("SELECT string_agg(portfolio_type || ':' || execution_type, ',' ORDER BY "
                     "portfolio_type, execution_type) FROM trading.executions WHERE portfolio_id = " +
                     lit(pid_)),
              "qt:ROLL,system:STRATEGY");
    EXPECT_EQ(scalar("SELECT COALESCE(book_source, 'NULL') FROM trading.live_results WHERE "
                     "portfolio_id = " + lit(pid_)),
              "NULL");
}

// Item 3: publish waits for a complete qt day and a finished desk command.
TEST_F(QtDeskDb, PublishNeedsTheMarkerAndAFinishedLastDeskCommand) {
    auto blocked = qt::publish_blocker(*db_, pid_, kSid, kDate);
    ASSERT_TRUE(blocked.is_ok());
    EXPECT_NE(blocked.value().find("no qt book"), std::string::npos) << blocked.value();

    const std::unordered_map<std::string, double> m{{"current_portfolio_value", 500000.0}};
    ASSERT_TRUE(db_->store_live_results_complete(kSid, day_at(), m, {}, nlohmann::json(), pid_,
                                                 "trading.live_results", nlohmann::json(), "qt")
                    .is_ok());
    blocked = qt::publish_blocker(*db_, pid_, kSid, kDate);
    ASSERT_TRUE(blocked.is_ok());
    EXPECT_NE(blocked.value().find("incomplete"), std::string::npos) << blocked.value();

    scalar("UPDATE trading.live_results SET book_source = 'desk' WHERE portfolio_id = " + lit(pid_) +
           " RETURNING 1");
    blocked = qt::publish_blocker(*db_, pid_, kSid, kDate);
    ASSERT_TRUE(blocked.is_ok());
    EXPECT_EQ(blocked.value(), "") << "a marked model or desk day with no command publishes";

    const long save = command("save");
    move(save, "running");
    blocked = qt::publish_blocker(*db_, pid_, kSid, kDate);
    EXPECT_NE(blocked.value().find("is running"), std::string::npos) << blocked.value();
    ASSERT_TRUE(qt::finish_audit_row(*db_, save, "failed", nlohmann::json::object(), "x").is_ok());
    blocked = qt::publish_blocker(*db_, pid_, kSid, kDate);
    EXPECT_NE(blocked.value().find("is failed"), std::string::npos) << blocked.value();

    const long again = command("save");
    move(again, "running");
    ASSERT_TRUE(qt::finish_audit_row(*db_, again, "done", nlohmann::json::object(), "ok").is_ok());
    blocked = qt::publish_blocker(*db_, pid_, kSid, kDate);
    EXPECT_EQ(blocked.value(), "");
}
