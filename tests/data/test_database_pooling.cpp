#include <gtest/gtest.h>
#include "trade_ngin/data/postgres_database.hpp"
#include <thread>
#include <future>

using namespace trade_ngin;
using namespace trade_ngin::testing;

class DatabasePoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a pool of database connections
        for (int i = 0; i < pool_size_; ++i) {
            auto db = std::make_shared<PostgresDatabase>(
                "postgresql://test:test@localhost:5432/testdb"
            );
            auto result = db->connect();
            if (result.is_ok()) {
                connection_pool_.push_back(db);
            }
        }
    }

    void TearDown() override {
        for (auto& db : connection_pool_) {
            if (db->is_connected()) {
                db->disconnect();
            }
        }
        connection_pool_.clear();
    }

    const int pool_size_ = 5;
    std::vector<std::shared_ptr<PostgresDatabase>> connection_pool_;
};

TEST_F(DatabasePoolTest, ConnectionPoolBasics) {
    ASSERT_EQ(connection_pool_.size(), pool_size_);
    
    for (const auto& db : connection_pool_) {
        EXPECT_TRUE(db->is_connected());
    }
}

TEST_F(DatabasePoolTest, ParallelQueries) {
    // Run multiple queries in parallel using the connection pool
    std::vector<std::future<Result<std::shared_ptr<arrow::Table>>>> futures;
    
    for (int i = 0; i < pool_size_ * 2; ++i) {
        auto db = connection_pool_[i % pool_size_];
        futures.push_back(std::async(std::launch::async, [db]() {
            return db->get_market_data(
                {"AAPL"},
                std::chrono::system_clock::now() - std::chrono::hours(24),
                std::chrono::system_clock::now(),
                AssetClass::EQUITIES,
                DataFrequency::DAILY
            );
        }));
    }

    // Verify all queries complete successfully
    for (auto& future : futures) {
        auto result = future.get();
        EXPECT_TRUE(result.is_ok());
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
    std::atomic<int> query_counts[5] = {0, 0, 0, 0, 0};
    std::vector<std::thread> threads;

    // Run many queries and count how many each connection handles
    for (int i = 0; i < 100; ++i) {
        threads.emplace_back([this, &query_counts, i]() {
            int conn_idx = i % pool_size_;
            auto db = connection_pool_[conn_idx];
            
            auto result = db->get_symbols(AssetClass::EQUITIES, DataFrequency::DAILY);
            if (result.is_ok()) {
                query_counts[conn_idx]++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Check that queries were reasonably distributed
    int min_count = std::numeric_limits<int>::max();
    int max_count = 0;
    for (int i = 0; i < pool_size_; ++i) {
        min_count = std::min(min_count, query_counts[i].load());
        max_count = std::max(max_count, query_counts[i].load());
    }

    // Verify the load is somewhat balanced (max difference of 50%)
    EXPECT_LT(max_count - min_count, min_count * 0.5);
}

TEST_F(DatabasePoolTest, QueryRetryBehavior) {
    int retry_count = 0;
    const int max_retries = 3;

    // Simulate a query that fails initially but succeeds after retries
    auto result = [&]() -> Result<void> {
        while (retry_count < max_retries) {
            retry_count++;
            if (retry_count >= 2) { // Succeed on second try
                return Result<void>();
            }
            // Simulate failure
            return make_error<void>(
                ErrorCode::DATABASE_ERROR,
                "Simulated failure",
                "DatabasePoolTest"
            );
        }
        return make_error<void>(
            ErrorCode::DATABASE_ERROR,
            "Max retries exceeded",
            "DatabasePoolTest"
        );
    }();

    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(retry_count, 2); // Should succeed on second try
}

TEST_F(DatabasePoolTest, ConnectionTimeout) {
    // Create a database with a very short timeout
    PostgresDatabase short_timeout_db(
        "postgresql://test:test@localhost:5432/testdb?connect_timeout=1"
    );

    // Try to connect to an invalid host
    auto start_time = std::chrono::steady_clock::now();
    auto result = short_timeout_db.connect();
    auto duration = std::chrono::steady_clock::now() - start_time;

    EXPECT_TRUE(result.is_error());
    // Verify timeout was respected
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::seconds>(duration).count(),
        2  // Should timeout in ~1 second
    );
}

TEST_F(DatabasePoolTest, ConnectionPoolExhaustion) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    std::atomic<int> failure_count(0);

    // Try to use more connections than available in the pool
    const int excess_threads = pool_size_ * 2;
    
    for (int i = 0; i < excess_threads; ++i) {
        threads.emplace_back([this, &success_count, &failure_count, i]() {
            auto db = connection_pool_[i % pool_size_];
            auto result = db->get_market_data(
                {"AAPL"},
                std::chrono::system_clock::now() - std::chrono::hours(24),
                std::chrono::system_clock::now(),
                AssetClass::EQUITIES,
                DataFrequency::DAILY
            );

            if (result.is_ok()) {
                success_count++;
            } else {
                failure_count++;
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify that some queries succeeded and some failed
    EXPECT_GT(success_count, 0);
    EXPECT_GT(failure_count, 0);
}