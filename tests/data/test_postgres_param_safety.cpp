#include <gtest/gtest.h>

#include <string>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;

// Phase 5 §5b -- pins validate_identifier's allowlist.
//
// SQL identifiers (table/column names) cannot be $-bound in Postgres -- they
// MUST be string-concatenated. validate_identifier is the chokepoint that
// rejects anything outside `[A-Za-z_][A-Za-z0-9_.]*` so a hostile string
// caller can't slip a `; DROP TABLE` into a query.
//
// We exercise validate_identifier without a live DB connection by
// constructing a PostgresDatabase with a bogus connection string -- the
// validator is a pure-string method that doesn't touch the connection
// pointer.

namespace {

PostgresDatabase make_offline_db() {
    // Bogus connection string -- validate_identifier doesn't dereference
    // the connection, so this never opens a socket.
    return PostgresDatabase("dbname=trade_ngin_test_offline");
}

}  // namespace

TEST(PostgresParamSafety, ValidIdentifiersAccepted) {
    PostgresDatabase db = make_offline_db();
    EXPECT_TRUE(db.validate_identifier("symbol").is_ok());
    EXPECT_TRUE(db.validate_identifier("trading.positions").is_ok());
    EXPECT_TRUE(db.validate_identifier("equities_data.fundamentals_1d").is_ok());
    EXPECT_TRUE(db.validate_identifier("_underscore_start").is_ok());
    EXPECT_TRUE(db.validate_identifier("col_with_digits_123").is_ok());
}

TEST(PostgresParamSafety, EmptyIdentifierRejected) {
    PostgresDatabase db = make_offline_db();
    auto r = db.validate_identifier("");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST(PostgresParamSafety, LeadingDigitRejected) {
    PostgresDatabase db = make_offline_db();
    auto r = db.validate_identifier("1bad");
    ASSERT_TRUE(r.is_error());
}

TEST(PostgresParamSafety, SemicolonInjectionRejected) {
    PostgresDatabase db = make_offline_db();
    auto r = db.validate_identifier("foo; DROP TABLE x");
    ASSERT_TRUE(r.is_error());
}

TEST(PostgresParamSafety, SingleQuoteRejected) {
    PostgresDatabase db = make_offline_db();
    auto r = db.validate_identifier("foo' OR 1=1 --");
    ASSERT_TRUE(r.is_error());
}

TEST(PostgresParamSafety, WhitespaceRejected) {
    PostgresDatabase db = make_offline_db();
    EXPECT_TRUE(db.validate_identifier("foo bar").is_error());
    EXPECT_TRUE(db.validate_identifier("foo\tbar").is_error());
    EXPECT_TRUE(db.validate_identifier("foo\nbar").is_error());
}

TEST(PostgresParamSafety, NonAsciiRejected) {
    PostgresDatabase db = make_offline_db();
    // "foo" + UTF-8 bytes for "é" (0xC3 0xA9) + "bar". Use string-literal
    // concatenation so the hex escapes don't greedy-eat the trailing chars.
    auto r = db.validate_identifier(std::string("foo") + "\xc3\xa9" + "bar");
    ASSERT_TRUE(r.is_error());
}

TEST(PostgresParamSafety, OversizeRejected) {
    PostgresDatabase db = make_offline_db();
    std::string huge(65, 'a');
    auto r = db.validate_identifier(huge);
    ASSERT_TRUE(r.is_error());
}

// Existing validate_strategy_id has slightly different rules (allows dash,
// 50-char cap). Pin those to ensure Phase 5's new helper doesn't accidentally
// converge them (they intentionally differ -- strategy_id is a value, not
// an identifier).
TEST(PostgresParamSafety, ValidateStrategyIdAcceptsDash) {
    PostgresDatabase db = make_offline_db();
    EXPECT_TRUE(db.validate_strategy_id("LIVE-EQUITY-MR").is_ok());
}

TEST(PostgresParamSafety, ValidateIdentifierRejectsDash) {
    PostgresDatabase db = make_offline_db();
    // identifier rules are stricter -- dashes are not valid SQL identifiers.
    auto r = db.validate_identifier("trading-positions");
    ASSERT_TRUE(r.is_error());
}
