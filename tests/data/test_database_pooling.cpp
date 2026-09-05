#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <thread>
#include <vector>
#include "../core/test_base.hpp"
#include "test_db_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

#include <chrono>
#include "trade_ngin/data/database_pooling.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class DatabasePoolTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        pool_size_ = 5;  // Define the size of the pool for testing

        // Create a pool of mock databases
        for (int i = 0; i < pool_size_; ++i) {
            auto db = std::make_shared<MockPostgresDatabase>("mock://testdb" + std::to_string(i));
            auto result = db->connect();
            ASSERT_TRUE(result.is_ok()) << "Failed to connect database " << i;
            connection_pool_.push_back(db);
        }
    }

    void TearDown() override {
        // Properly disconnect and clear the pool
        for (auto& db : connection_pool_) {
            if (db->is_connected()) {
                db->disconnect();
            }
        }
        connection_pool_.clear();
        TestBase::TearDown();
    }

    int pool_size_;  // Size of the connection pool
    std::vector<std::shared_ptr<MockPostgresDatabase>> connection_pool_;
};

TEST_F(DatabasePoolTest, ConnectionPoolBasics) {
    ASSERT_EQ(connection_pool_.size(), pool_size_);

    // Verify all connections are active
    for (const auto& db : connection_pool_) {
        EXPECT_TRUE(db->is_connected());
    }
}

TEST_F(DatabasePoolTest, ParallelQueries) {
    struct QueryResult {
        bool success{false};
        std::string error_message;
        size_t num_rows{0};
    };

    std::vector<QueryResult> results(pool_size_ * 2);
    std::vector<std::thread> threads;
    std::mutex results_mutex;

    auto start_time = std::chrono::system_clock::now() - std::chrono::hours(24);
    auto end_time = std::chrono::system_clock::now();

    // Run multiple market data queries in parallel
    for (int i = 0; i < pool_size_ * 2; ++i) {
        threads.emplace_back([&, i]() {
            auto db = connection_pool_[i % pool_size_];
            auto query_result = db->get_market_data({"AAPL", "MSFT"}, start_time, end_time,
                                                    AssetClass::EQUITIES, DataFrequency::DAILY);

            QueryResult thread_result;
            if (query_result.is_ok()) {
                thread_result.success = true;
                thread_result.num_rows = query_result.value()->num_rows();
            } else {
                thread_result.success = false;
                thread_result.error_message = query_result.error()->what();
            }

            // Store result in the results vector
            std::lock_guard<std::mutex> lock(results_mutex);
            results[i] = thread_result;
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify results
    for (const auto& result : results) {
        EXPECT_TRUE(result.success) << "Query failed: " << result.error_message;
        if (result.success) {
            EXPECT_GT(result.num_rows, 0);
        }
    }
}

TEST_F(DatabasePoolTest, ConnectionFailureRecovery) {
    // Simulate a connection failure
    connection_pool_[0]->disconnect();
    EXPECT_FALSE(connection_pool_[0]->is_connected());

    // Attempt to reconnect
    auto result = connection_pool_[0]->connect();
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(connection_pool_[0]->is_connected());
}

TEST_F(DatabasePoolTest, LoadBalancing) {
    std::vector<std::atomic<int>> query_counts(pool_size_);
    std::vector<std::thread> threads;
    const int num_queries = 100;

    // Run many queries and count distribution
    for (int i = 0; i < num_queries; ++i) {
        threads.emplace_back([this, &query_counts, i]() {
            int conn_idx = i % pool_size_;
            auto db = connection_pool_[conn_idx];

            auto result = db->get_market_data(
                {"AAPL"}, std::chrono::system_clock::now() - std::chrono::hours(24),
                std::chrono::system_clock::now(), AssetClass::EQUITIES, DataFrequency::DAILY);

            if (result.is_ok()) {
                query_counts[conn_idx]++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Analyze distribution
    int min_count = std::numeric_limits<int>::max();
    int max_count = 0;
    for (int i = 0; i < pool_size_; ++i) {
        min_count = std::min(min_count, query_counts[i].load());
        max_count = std::max(max_count, query_counts[i].load());
    }

    // Verify reasonable load distribution (max difference < 50%)
    double imbalance = static_cast<double>(max_count - min_count) / min_count;
    EXPECT_LT(imbalance, 0.5) << "Load distribution is too uneven";
}

TEST_F(DatabasePoolTest, ConcurrentStateChanges) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    const int num_threads = pool_size_ * 2;

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, &success_count, i]() {
            auto db = connection_pool_[i % pool_size_];

            // Randomly disconnect and reconnect
            if (i % 2 == 0) {
                db->disconnect();
                auto result = db->connect();
                if (result.is_ok()) {
                    success_count++;
                }
            } else {
                auto result = db->get_market_data(
                    {"AAPL"}, std::chrono::system_clock::now() - std::chrono::hours(24),
                    std::chrono::system_clock::now(), AssetClass::EQUITIES, DataFrequency::DAILY);
                if (result.is_ok()) {
                    success_count++;
                }
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_GT(success_count, 0);
}

TEST_F(DatabasePoolTest, ErrorPropagation) {
    // Test error propagation in multi-threaded context
    struct ErrorTestResult {
        bool is_error{false};
        ErrorCode error_code{ErrorCode::NONE};
        std::string error_message;
    };

    std::vector<ErrorTestResult> results(pool_size_);
    std::vector<std::thread> threads;
    std::mutex results_mutex;

    for (int i = 0; i < pool_size_; ++i) {
        threads.emplace_back([&, i]() {
            auto db = connection_pool_[i];
            auto result = db->get_market_data(
                {"INVALID_SYMBOL"},  // Should trigger an error
                std::chrono::system_clock::now(),
                std::chrono::system_clock::now() - std::chrono::hours(24),  // Invalid time range
                AssetClass::EQUITIES);

            ErrorTestResult thread_result;
            thread_result.is_error = result.is_error();
            if (result.is_error()) {
                thread_result.error_code = result.error()->code();
                thread_result.error_message = result.error()->what();
            }

            // Store result thread-safely
            std::lock_guard<std::mutex> lock(results_mutex);
            results[i] = thread_result;
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify all errors are properly propagated
    for (const auto& result : results) {
        EXPECT_TRUE(result.is_error) << "Expected error but got success";
        if (result.is_error) {
            EXPECT_EQ(result.error_code, ErrorCode::INVALID_ARGUMENT)
                << "Unexpected error code: " << static_cast<int>(result.error_code)
                << " with message: " << result.error_message;
        }
    }
}

TEST_F(DatabasePoolTest, MixedOperations) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    const int operations_per_type = pool_size_;

    // Market data queries
    for (int i = 0; i < operations_per_type; ++i) {
        threads.emplace_back([this, &success_count]() {
            auto db = connection_pool_[rand() % pool_size_];
            auto result = db->get_market_data(
                {"AAPL"}, std::chrono::system_clock::now() - std::chrono::hours(24),
                std::chrono::system_clock::now(), AssetClass::EQUITIES);
            if (result.is_ok())
                success_count++;
        });
    }

    // Store executions
    for (int i = 0; i < operations_per_type; ++i) {
        threads.emplace_back([this, &success_count]() {
            auto db = connection_pool_[rand() % pool_size_];
            auto result =
                db->store_executions(create_test_executions(), "TEST_STRATEGY", "TEST_STRATEGY",
                                     "BASE_PORTFOLIO", "trading.executions");
            if (result.is_ok())
                success_count++;
        });
    }

    // Store positions
    for (int i = 0; i < operations_per_type; ++i) {
        threads.emplace_back([this, &success_count]() {
            auto db = connection_pool_[rand() % pool_size_];
            auto result =
                db->store_positions(create_test_positions(), "TEST_STRATEGY", "TEST_STRATEGY",
                                    "BASE_PORTFOLIO", "trading.positions");
            if (result.is_ok())
                success_count++;
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Expect reasonable success rate
    double success_rate = static_cast<double>(success_count) / (operations_per_type * 3);
    EXPECT_GT(success_rate, 0.8) << "Too many operations failed";
}

// ===== folded in from tests/data/test_database_pooling_extended.cpp =====
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
namespace database_pooling_extended_detail {

using namespace trade_ngin;

class DatabasePoolingExtendedTest : public ::testing::Test {};

// The retry paths are now exercised below. They previously had to be skipped
// because retry_with_backoff called std::rand(), advancing global RNG state and
// breaking downstream tests that also used rand(). backoff_jitter() now owns a
// thread_local engine, so the retry paths no longer have cross-test side effects.

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

// ===== backoff jitter =====

// The jitter exists to decorrelate clients competing for the same database.
// An unseeded rand() emitted an identical sequence in every process, so all
// clients retried in lockstep. Each thread must now draw its own sequence.
TEST_F(DatabasePoolingExtendedTest, BackoffJitterStaysWithinDocumentedRange) {
    for (int i = 0; i < 200; ++i) {
        auto jitter = utils::backoff_jitter();
        EXPECT_GE(jitter.count(), 0);
        EXPECT_LE(jitter.count(), 99);
    }
}

TEST_F(DatabasePoolingExtendedTest, BackoffJitterDecorrelatesAcrossThreads) {
    // Compare whole sequences rather than single draws: two threads each drawing
    // one value from [0, 99] would collide ~1% of the time, which would flake.
    constexpr int kDraws = 8;
    std::vector<long long> a;
    std::vector<long long> b;

    auto collect = [](std::vector<long long>& out) {
        for (int i = 0; i < kDraws; ++i) {
            out.push_back(utils::backoff_jitter().count());
        }
    };

    std::thread t1(collect, std::ref(a));
    std::thread t2(collect, std::ref(b));
    t1.join();
    t2.join();

    ASSERT_EQ(a.size(), static_cast<size_t>(kDraws));
    ASSERT_EQ(b.size(), static_cast<size_t>(kDraws));
    // Two independently seeded mt19937 engines matching on all 8 draws has
    // probability ~100^-8; any match means the engines share a seed sequence.
    EXPECT_NE(a, b) << "Both threads produced an identical jitter sequence, "
                       "so retry timing is still correlated across threads.";
}

// ===== retry path =====

TEST_F(DatabasePoolingExtendedTest, RetryWithBackoffRetriesOnConnectionError) {
    std::atomic<int> calls{0};
    auto fn = [&]() -> Result<int> {
        ++calls;
        return make_error<int>(ErrorCode::CONNECTION_ERROR, "retryable", "test");
    };

    auto start = std::chrono::steady_clock::now();
    auto r = utils::retry_with_backoff(fn, 2);
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_TRUE(r.is_error());
    // max_retries attempts inside the loop, then one final attempt.
    EXPECT_EQ(calls.load(), 3);
    // Backoff must remain exponential: the loop sleeps 30ms then >=60ms.
    // Asserting only the lower bound keeps this robust on a loaded CI runner.
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 90);
}

TEST_F(DatabasePoolingExtendedTest, RetryWithBackoffReturnsSuccessAfterTransientFailure) {
    std::atomic<int> calls{0};
    auto fn = [&]() -> Result<int> {
        if (++calls == 1) {
            return make_error<int>(ErrorCode::CONNECTION_ERROR, "transient", "test");
        }
        return Result<int>(7);
    };
    auto r = utils::retry_with_backoff(fn, 3);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 7);
    EXPECT_EQ(calls.load(), 2);
}

}  // namespace database_pooling_extended_detail
