/**
 * Regression tests for the SQL-injection hardening (PR #42).
 *
 * Two guards under test:
 *  1. build_value_placeholders() — the multi-row parameterized-INSERT builder
 *     behind the chunked execution-storage path. Its invariants (contiguous
 *     $n numbering, rows*cols parameters, well-formed groups) are what keep a
 *     placeholder/param count mismatch from silently corrupting stored rows.
 *  2. The dynamic-column identifier whitelist in update_live_results /
 *     store_live_results_complete — the only defense available for
 *     identifiers, which cannot be bound as parameters. Validation runs ahead
 *     of the connection check, so these paths are exercised offline through
 *     the real implementation.
 */
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include "test_db_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// ---------------------------------------------------------------------------
// build_value_placeholders
// ---------------------------------------------------------------------------

TEST(BuildValuePlaceholders, SingleRowSingleColumn) {
    EXPECT_EQ(detail::build_value_placeholders(1, 1), "($1)");
}

TEST(BuildValuePlaceholders, SingleRowManyColumns) {
    EXPECT_EQ(detail::build_value_placeholders(1, 3), "($1,$2,$3)");
}

TEST(BuildValuePlaceholders, NumberingContinuesAcrossRows) {
    // A break in numbering here is exactly the off-by-one that would bind
    // values into the wrong columns of the following row.
    EXPECT_EQ(detail::build_value_placeholders(2, 2), "($1,$2),($3,$4)");
    EXPECT_EQ(detail::build_value_placeholders(3, 2), "($1,$2),($3,$4),($5,$6)");
}

TEST(BuildValuePlaceholders, ParameterCountMatchesRowsTimesColumns) {
    // The >100-execution path chunks at 1000 rows; check a full chunk at the
    // real column width (9 columns per execution row).
    const size_t rows = 1000, cols = 9;
    std::string sql = detail::build_value_placeholders(rows, cols);

    EXPECT_EQ(static_cast<size_t>(std::count(sql.begin(), sql.end(), '$')), rows * cols);
    EXPECT_EQ(static_cast<size_t>(std::count(sql.begin(), sql.end(), '(')), rows);
    EXPECT_EQ(static_cast<size_t>(std::count(sql.begin(), sql.end(), ')')), rows);

    // Highest parameter index must be exactly rows*cols and stay under
    // Postgres's 65535-parameter cap for the chunk sizes callers use.
    std::string last = "$" + std::to_string(rows * cols) + ")";
    ASSERT_GE(sql.size(), last.size());
    EXPECT_EQ(sql.compare(sql.size() - last.size(), last.size(), last), 0);
    EXPECT_LE(rows * cols, 65535u);
}

TEST(BuildValuePlaceholders, ContainsNoValueText) {
    // The whole point: nothing but placeholders and punctuation may reach the
    // query string from this builder.
    std::string sql = detail::build_value_placeholders(4, 5);
    for (char c : sql) {
        bool allowed = c == '$' || c == ',' || c == '(' || c == ')' || (c >= '0' && c <= '9');
        ASSERT_TRUE(allowed) << "unexpected character in placeholder SQL: " << c;
    }
}

// ---------------------------------------------------------------------------
// Dynamic-column identifier whitelist (through the real public methods)
// ---------------------------------------------------------------------------

class IdentifierWhitelistTest : public ::testing::Test {
protected:
    void SetUp() override {
        db = std::make_unique<MockPostgresDatabase>("mock://testdb");
        db->connect();
    }
    std::unique_ptr<MockPostgresDatabase> db;

    Result<void> update_with_column(const std::string& column) {
        std::unordered_map<std::string, double> updates{{column, 1.0}};
        // Qualified call: MockPostgresDatabase overrides update_live_results
        // with a stub, and the whitelist under test lives in the REAL
        // implementation. Its input validation runs before the connection
        // check, so this is safe against the mock's absent connection.
        return db->PostgresDatabase::update_live_results(
            "trend_following", std::chrono::system_clock::now(), updates, "BASE_PORTFOLIO",
            "trading.live_results");
    }
};

TEST_F(IdentifierWhitelistTest, RejectsInjectionShapedColumnNames) {
    const char* attacks[] = {
        "equity = 0; DROP TABLE trading.positions--",
        "equity'--",
        "equity\"; DELETE FROM trading.live_results; --",
        "equity, portfolio_id = 'x",
        "equity)::text||'",
    };
    for (const char* column : attacks) {
        auto result = update_with_column(column);
        ASSERT_TRUE(result.is_error()) << "accepted: " << column;
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT) << column;
    }
}

TEST_F(IdentifierWhitelistTest, RejectsMalformedIdentifiers) {
    EXPECT_EQ(update_with_column("").error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(update_with_column("1starts_with_digit").error()->code(),
              ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(update_with_column("has space").error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(update_with_column(std::string(64, 'a')).error()->code(),
              ErrorCode::INVALID_ARGUMENT);
}

TEST_F(IdentifierWhitelistTest, ValidColumnPassesWhitelistAndStopsAtConnection) {
    // The mock reports connected but holds no live pqxx connection, so a safe
    // column name must get PAST the whitelist and be stopped by the connection
    // check -- proving the rejection above is the whitelist, not the mock.
    auto result = update_with_column("total_pnl");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::CONNECTION_ERROR);
}

TEST_F(IdentifierWhitelistTest, CompleteStoreRejectsBadMetricNames) {
    std::unordered_map<std::string, double> metrics{{"equity; --", 1.0}};
    std::unordered_map<std::string, int> int_metrics;
    auto result = db->PostgresDatabase::store_live_results_complete(
        "trend_following", std::chrono::system_clock::now(), metrics, int_metrics,
        nlohmann::json::object(), "BASE_PORTFOLIO", "trading.live_results");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(IdentifierWhitelistTest, CompleteStoreRejectsBadIntMetricNames) {
    std::unordered_map<std::string, double> metrics;
    std::unordered_map<std::string, int> int_metrics{{"trades'; DROP TABLE x--", 1}};
    auto result = db->PostgresDatabase::store_live_results_complete(
        "trend_following", std::chrono::system_clock::now(), metrics, int_metrics,
        nlohmann::json::object(), "BASE_PORTFOLIO", "trading.live_results");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// ---------------------------------------------------------------------------
// Destructor exception safety (S1048 fix)
// ---------------------------------------------------------------------------

TEST(DestructorExceptionSafety, NoThrowOnDestruction) {
    // Verify destructor doesn't throw even if disconnect fails
    auto db_ptr = std::make_unique<MockPostgresDatabase>("mock://testdb");
    db_ptr->connect();
    // Destructor should not throw
    db_ptr.reset();  // Calls destructor - should not throw
    SUCCEED();  // If we got here, no exception was thrown
}

// ---------------------------------------------------------------------------
// Valid SQL identifiers (std::ranges fix)
// ---------------------------------------------------------------------------

TEST_F(IdentifierWhitelistTest, AcceptsValidIdentifiers) {
    EXPECT_EQ(update_with_column("total_pnl").error()->code(),
              ErrorCode::CONNECTION_ERROR);  // Passes whitelist, fails on connection
    EXPECT_EQ(update_with_column("equity_2023").error()->code(), ErrorCode::CONNECTION_ERROR);
    EXPECT_EQ(update_with_column("_hidden_column").error()->code(), ErrorCode::CONNECTION_ERROR);
    EXPECT_EQ(update_with_column("a").error()->code(), ErrorCode::CONNECTION_ERROR);
    EXPECT_EQ(update_with_column("SharePrice").error()->code(), ErrorCode::CONNECTION_ERROR);
}
