// E-2 DB-session-timezone-pin. PostgresDatabase::connect pins the session's time
// zone to UTC, so the text of a timestamptz read on it does not follow the
// database's default, the role's, or the client's PGTZ.
//
// Two tests. The first runs with no database: it reads the statement connect()
// issues. The second needs TRADE_NGIN_TEST_DSN: it sets PGTZ=America/New_York in
// this process (libpq sends it as the session's start-up time zone), connects,
// and reads the session's zone back. Without the pin the session reads
// America/New_York. Nothing is written and no setting of the database changes.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include <arrow/api.h>

#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

namespace {

// Sets one environment variable for the life of the object and puts the
// previous state back.
struct ScopedEnv {
    std::string name;
    std::string previous;
    bool had_previous = false;
    ScopedEnv(const char* n, const char* value) : name(n) {
        if (const char* p = std::getenv(n)) {
            previous = p;
            had_previous = true;
        }
        setenv(n, value, 1);
    }
    ~ScopedEnv() {
        if (had_previous) {
            setenv(name.c_str(), previous.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

std::string first_cell(PostgresDatabase& db, const std::string& query) {
    auto r = db.execute_query(query);
    if (r.is_error())
        return std::string("ERROR: ") + r.error()->what();
    auto table = r.value();
    if (!table || table->num_rows() != 1 || table->num_columns() != 1)
        return "ERROR: shape";
    auto scalar = table->column(0)->GetScalar(0);
    if (!scalar.ok())
        return "ERROR: scalar";
    return (*scalar)->ToString();
}

}  // namespace

TEST(SessionTimeZone, TheStatementConnectIssuesPinsUtc) {
    EXPECT_STREQ(PostgresDatabase::session_time_zone_statement(), "SET TimeZone = 'UTC'");
}

TEST(SessionTimeZone, ASessionOpenedUnderAnotherClientZoneReadsUtc) {
    const char* dsn = std::getenv("TRADE_NGIN_TEST_DSN");
    if (!dsn || !*dsn) {
        GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set; no database to exercise";
    }
    ScopedEnv pgtz("PGTZ", "America/New_York");

    PostgresDatabase db{std::string(dsn)};
    auto connected = db.connect();
    ASSERT_TRUE(connected.is_ok()) << connected.error()->what();

    EXPECT_EQ(first_cell(db, "SHOW TimeZone"), "UTC")
        << "the session took the client's PGTZ: connect() did not pin it";
    // What the pin is for: a bar stored at midnight UTC reads back as that text.
    EXPECT_EQ(first_cell(db, "SELECT (TIMESTAMPTZ '2026-04-24 00:00:00+00')::text"),
              "2026-04-24 00:00:00+00");

    db.disconnect();
}
