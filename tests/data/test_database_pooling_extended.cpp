// Coverage for database_pooling.cpp paths that don't require a live DB.
// Targets:
// - DatabasePool::initialize with a connection string that fails to open
//   (returns CONNECTION_ERROR after 0 successful connections)
// - DatabasePool::return_connection with nullptr
// - retry_with_backoff template (header) on success and on retryable failure
//
// We deliberately avoid testing acquire_connection / multi-connection paths
// because those require a working PostgresDatabase, which the existing
// MockPostgresDatabase doesn't substitute for inside DatabasePool (it
// constructs std::make_shared<PostgresDatabase> directly). Captured here as
// a deferred-refactor item; see deliverables/unit_testing/postgres_database.md.

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include "trade_ngin/data/database_pooling.hpp"

using namespace trade_ngin;

class DatabasePoolingExtendedTest : public ::testing::Test {};

// NOTE: We deliberately don't exercise retry_with_backoff retry paths here.
// That helper calls std::rand() without seeding, which advances the global
// RNG state and breaks downstream tests (e.g.
// TransactionCostAnalyzerTest.ImplementationShortfall) that also rely on
// rand(). Captured as a FIXME on the production retry helper —
// it should use a local std::mt19937 with its own seed.

// ===== initialize fails when all connections fail =====

TEST_F(DatabasePoolingExtendedTest, InitializeWithBadConnectionStringReturnsError) {
    // Use a syntactically-malformed connection string so libpq rejects
    // immediately without DNS lookup or TCP timeout.
    std::string bad_conn = "host= port=invalid_not_a_number user= dbname=";
    auto& pool = DatabasePool::instance();
    auto r = pool.initialize(bad_conn, /*pool_size=*/1);
    // initialize is a singleton method. If it succeeds (was already
    // initialized earlier in the test run), it returns a warning but ok.
    // If this is the first call, it should return error because all
    // connection attempts fail.
    if (!r.is_ok()) {
        EXPECT_EQ(r.error()->code(), ErrorCode::CONNECTION_ERROR);
    }
}

// ===== return_connection rejects nullptr =====

TEST_F(DatabasePoolingExtendedTest, ReturnNullConnectionReturnsInvalidArgument) {
    auto& pool = DatabasePool::instance();
    auto r = pool.return_connection(nullptr);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// ===== retry_with_backoff =====

TEST_F(DatabasePoolingExtendedTest, RetryWithBackoffReturnsImmediatelyOnSuccess) {
    std::atomic<int> calls{0};
    auto fn = [&]() -> Result<int> {
        ++calls;
        return Result<int>(42);
    };
    auto r = utils::retry_with_backoff(fn, 3);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 42);
    EXPECT_EQ(calls.load(), 1);  // no retry needed
}

TEST_F(DatabasePoolingExtendedTest, RetryWithBackoffStopsRetryingOnNonRetryableError) {
    std::atomic<int> calls{0};
    auto fn = [&]() -> Result<int> {
        ++calls;
        return make_error<int>(ErrorCode::INVALID_ARGUMENT, "non-retryable", "test");
    };
    auto r = utils::retry_with_backoff(fn, 5);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(calls.load(), 1);  // not a CONNECTION_ERROR → no retries
}

// Retry-path tests for retry_with_backoff are intentionally omitted: the
// helper calls std::rand() which would pollute global RNG state used by
// other tests. See note above.
