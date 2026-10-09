// DB-load-positions-no-validate: load_positions_by_date interpolated `table_name`
// straight into its FROM clause without going through validate_table_name, while
// every other interpolating read in postgres_database.cpp validates first.
//
// Nothing exploits it today, and the reason is not this function: it is that all
// four callers happen to pass the literal "trading.positions". That is a property
// of today's call sites. The next caller to pass a name derived from config, a
// portfolio id, or an asset-class switch inherits an unvalidated concatenation
// into SQL, and the read is the one that seeds the whole live book.
//
// Proving the check exists needs a real connection, because the validation sits
// after validate_connection() exactly as it does in the sibling methods: offline
// the function returns CONNECTION_ERROR before it ever looks at the table name,
// so an offline test would pass without the fix. Hence a DB-backed test.
//
// Reachability gate matches tests/data/test_delete_stale_executions_scope_db.cpp:
//   * TRADE_NGIN_REQUIRE_DB=1 -- unreachable database FAILS rather than skips.
//   * unset -- skip (local dev without a server).
//
// Read-only: every case is expected to be rejected before any SQL is issued, and
// the one accepted case reads a scratch identity that owns no rows.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

std::string discover_connection_string() {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (dsn && *dsn) return std::string(dsn);
    return {};
}

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

}  // namespace

class LoadPositionsIdentifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        const bool require_db = [] {
            const char* v = std::getenv("TRADE_NGIN_REQUIRE_DB");
            return v && std::string(v) == "1";
        }();
        const std::string conn = discover_connection_string();
        if (conn.empty()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but TRADE_NGIN_TEST_DSN is not set, so the "
                          "load_positions_by_date identifier check goes unverified";
            }
            GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
        }
        db_ = std::make_shared<PostgresDatabase>(conn);
        auto connected = db_->connect();
        if (connected.is_error() || !db_->is_connected()) {
            if (require_db) {
                FAIL() << "TRADE_NGIN_REQUIRE_DB=1 but the database is unreachable";
            }
            GTEST_SKIP() << "database unreachable; skipping";
        }
    }

    void TearDown() override {
        if (db_ && db_->is_connected()) db_->disconnect();
    }

    Result<std::unordered_map<std::string, Position>> load(const std::string& table) {
        return db_->load_positions_by_date("T1_PROBE_ID", "T1_PROBE_NAME", "T1_PROBE_PORTFOLIO",
                                           date_at(2026, 5, 4), table);
    }

    std::shared_ptr<PostgresDatabase> db_;
};

// Before the fix each of these reached the server as part of a concatenated
// FROM clause. The first two are the SQL-injection shapes validate_table_name
// exists to stop; the rest are the malformed names it also rejects.
TEST_F(LoadPositionsIdentifierTest, HostileTableNameIsRejectedByTheValidator) {
    const char* hostile[] = {
        "trading.positions; DROP TABLE trading.positions",
        "trading.positions WHERE 1=1 UNION SELECT * FROM trading.live_results",
        "trading.positions--",
        "trading.positions/*x*/",
        "trading positions",
        "trading.\"positions\"",
        "",
    };
    for (const char* t : hostile) {
        auto r = load(t);
        ASSERT_TRUE(r.is_error()) << "accepted table_name: '" << t << "'";
        EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT)
            << "table_name '" << t << "' was refused, but not by the identifier allowlist -- "
            << "got: " << r.error()->what();
    }
}

// The counterpart that stops the test passing for the wrong reason: a legitimate
// name must still get through, so the check cannot be satisfied by refusing
// everything.
TEST_F(LoadPositionsIdentifierTest, LegitimateTableNameStillReads) {
    auto r = load("trading.positions");
    ASSERT_TRUE(r.is_ok()) << "the real table was refused: " << r.error()->what();
    EXPECT_TRUE(r.value().empty()) << "the scratch identity is not supposed to own any rows";
}
