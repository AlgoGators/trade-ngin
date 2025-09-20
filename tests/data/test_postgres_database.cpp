#include <arrow/type_traits.h>
#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>
#include "test_db_utils.hpp"
#include "trade_ngin/data/postgres_database.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class PostgresDatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create database instance with test connection string
        db = std::make_unique<trade_ngin::testing::MockPostgresDatabase>("mock://testdb");
    }

    void TearDown() override {
        if (db && db->is_connected()) {
            db->disconnect();
        }
    }

    std::unique_ptr<PostgresDatabase> db;
};

TEST_F(PostgresDatabaseTest, ConnectionLifecycle) {
    EXPECT_FALSE(db->is_connected());

    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());
    EXPECT_TRUE(db->is_connected());

    db->disconnect();
    EXPECT_FALSE(db->is_connected());
}

TEST_F(PostgresDatabaseTest, GetMarketData) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    std::vector<std::string> symbols = {"AAPL", "MSFT"};
    auto start_date = std::chrono::system_clock::now() - std::chrono::hours(24);
    auto end_date = std::chrono::system_clock::now();

    auto result = db->get_market_data(symbols, start_date, end_date, AssetClass::EQUITIES,
                                      DataFrequency::DAILY);

    ASSERT_TRUE(result.is_ok());
    auto table = result.value();
    EXPECT_GT(table->num_rows(), 0);
    EXPECT_EQ(table->num_columns(),
              7);  // time, symbol, open, high, low, close, volume
}

TEST_F(PostgresDatabaseTest, GetMarketDataInvalidDateRange) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    std::vector<std::string> symbols = {"AAPL"};
    auto end_date = std::chrono::system_clock::now() - std::chrono::hours(24);
    auto start_date = end_date + std::chrono::hours(48);  // Invalid: start after end

    auto result = db->get_market_data(symbols, start_date, end_date, AssetClass::EQUITIES,
                                      DataFrequency::DAILY);

    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(PostgresDatabaseTest, StoreExecutions) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    auto executions = create_test_executions();
    auto result = db->store_executions(executions, "trading.executions");

    ASSERT_TRUE(result.is_ok());
}

TEST_F(PostgresDatabaseTest, StorePositions) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    auto positions = create_test_positions();
    auto result = db->store_positions(positions, "TEST_STRATEGY", "trading.positions");

    ASSERT_TRUE(result.is_ok());
}

TEST_F(PostgresDatabaseTest, StoreSignals) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    std::unordered_map<std::string, double> signals = {{"AAPL", 1.5}, {"MSFT", -0.8}};

    auto result = db->store_signals(signals, "test_strategy", std::chrono::system_clock::now(),
                                    "trading.signals");

    ASSERT_TRUE(result.is_ok());
}

TEST_F(PostgresDatabaseTest, GetSymbols) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    auto result = db->get_symbols(AssetClass::EQUITIES, DataFrequency::DAILY);

    ASSERT_TRUE(result.is_ok());
    EXPECT_GT(result.value().size(), 0);
}

TEST_F(PostgresDatabaseTest, ExecuteCustomQuery) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    std::string query = R"(
        SELECT symbol, AVG(close) as avg_price
        FROM equities_data.ohlcv_1d
        WHERE time >= NOW() - INTERVAL '30 days'
        GROUP BY symbol
    )";

    auto result = db->execute_query(query);
    ASSERT_TRUE(result.is_ok());
    EXPECT_GT(result.value()->num_rows(), 0);
}

TEST_F(PostgresDatabaseTest, DisconnectedOperations) {
    // Ensure database is not connected
    ASSERT_FALSE(db->is_connected());

    // Don't connect, try operations
    auto result1 = db->get_market_data({"AAPL"}, std::chrono::system_clock::now(),
                                       std::chrono::system_clock::now(), AssetClass::EQUITIES,
                                       DataFrequency::DAILY);
    EXPECT_TRUE(result1.is_error());

    // Store executions
    ExecutionReport exec;
    auto result2 = db->store_executions({exec}, "trading.executions");
    EXPECT_TRUE(result2.is_error());
    EXPECT_EQ(result2.error()->code(), ErrorCode::DATABASE_ERROR);

    // Execute query
    auto result3 = db->execute_query("SELECT 1");
    EXPECT_TRUE(result3.is_error()) << "Query should fail without connection";
    EXPECT_EQ(result3.error()->code(), ErrorCode::DATABASE_ERROR);
}

TEST_F(PostgresDatabaseTest, InvalidTableNames) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    // Try to store executions in a non-existent table
    auto executions = create_test_executions();
    auto result = db->store_executions(executions, "invalid_schema.invalid_table");

    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);
}

TEST_F(PostgresDatabaseTest, ConcurrentAccess) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    // Create multiple threads that access the database simultaneously
    std::vector<std::thread> threads;
    std::atomic<int> success_count(0);
    constexpr int num_threads = 10;

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([this, &success_count]() {
            // Each thread performs a different database operation
            switch (success_count % 3) {
                case 0: {
                    auto result = db->get_market_data(
                        {"AAPL"}, std::chrono::system_clock::now() - std::chrono::hours(24),
                        std::chrono::system_clock::now(), AssetClass::EQUITIES,
                        DataFrequency::DAILY);
                    if (result.is_ok())
                        success_count++;
                    break;
                }
                case 1: {
                    auto positions = create_test_positions();
                    auto result =
                        db->store_positions(positions, "TEST_STRATEGY", "trading.positions");
                    if (result.is_ok())
                        success_count++;
                    break;
                }
                case 2: {
                    auto result = db->get_symbols(AssetClass::EQUITIES, DataFrequency::DAILY);
                    if (result.is_ok())
                        success_count++;
                    break;
                }
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify that at least some operations succeeded
    EXPECT_GT(success_count, 0);
}

// TEST_F(PostgresDatabaseTest, ConnectionTimeout) {
//     // Create database with invalid host to test timeout
//     PostgresDatabase timeout_db("postgresql://invalid_host:5432/testdb");
//
//     auto start_time = std::chrono::steady_clock::now();
//     auto result = timeout_db.connect();
//     auto duration = std::chrono::steady_clock::now() - start_time;
//
//     EXPECT_TRUE(result.is_error());
//     EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);
//
//     // Check that it didn't hang for too long (should timeout within reasonable
//     // time)
//     EXPECT_LT(std::chrono::duration_cast<std::chrono::seconds>(duration).count(), 5);
// }

TEST_F(PostgresDatabaseTest, ReconnectionBehavior) {
    // Test reconnection after disconnection
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());
    EXPECT_TRUE(db->is_connected());

    db->disconnect();
    EXPECT_FALSE(db->is_connected());

    // Try to reconnect
    connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());
    EXPECT_TRUE(db->is_connected());
}

TEST_F(PostgresDatabaseTest, LargeDatasetHandling) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    // Create a large dataset request
    std::vector<std::string> symbols;
    for (int i = 0; i < 100; ++i) {
        symbols.push_back("SYMBOL" + std::to_string(i));
    }

    auto start_date = std::chrono::system_clock::now() - std::chrono::hours(24 * 365);  // 1 year
    auto end_date = std::chrono::system_clock::now();

    auto result = db->get_market_data(symbols, start_date, end_date, AssetClass::EQUITIES,
                                      DataFrequency::DAILY);

    ASSERT_TRUE(result.is_ok());
    // Verify the Arrow table's memory usage is reasonable
    auto table = result.value();
    size_t estimated_size =
        table->num_rows() * table->num_columns() * sizeof(double);  // Rough estimate
    EXPECT_LT(estimated_size, 1024 * 1024 * 1024);                  // Less than 1GB
}

TEST_F(PostgresDatabaseTest, TransactionRollback) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    // First store some valid positions
    auto positions = create_test_positions();
    auto result = db->store_positions(positions, "TEST_STRATEGY", "trading.positions");
    ASSERT_TRUE(result.is_ok());

    // Try to store invalid positions (create a position with invalid values)
    Position invalid_pos;
    invalid_pos.symbol = std::string(1000, 'A');  // Too long for DB column
    positions.push_back(invalid_pos);

    result = db->store_positions(positions, "TEST_STRATEGY", "trading.positions");
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);

    // Verify the valid positions are still intact
    auto query_result = db->execute_query("SELECT COUNT(*) FROM trading.positions");
    ASSERT_TRUE(query_result.is_ok());

    // Get the scalar value from the result
    auto scalar_result = query_result.value()->column(0)->GetScalar(0);
    ASSERT_TRUE(scalar_result.ok());

    // Cast to specific scalar type
    auto count_scalar = std::static_pointer_cast<arrow::Int64Scalar>(scalar_result.ValueOrDie());

    // Now verify the value
    EXPECT_EQ(count_scalar->value, positions.size() - 1);
}

TEST_F(PostgresDatabaseTest, TimezoneHandling) {
    auto connect_result = db->connect();
    ASSERT_TRUE(connect_result.is_ok());

    // Test with explicit timezone in timestamps
    auto ny_time = std::chrono::system_clock::now();  // Assume America/New_York
    auto utc_time = ny_time + std::chrono::hours(4);  // Simulate UTC conversion

    auto result = db->get_market_data({"AAPL"}, ny_time, utc_time, AssetClass::EQUITIES,
                                      DataFrequency::DAILY);

    ASSERT_TRUE(result.is_ok());
    // Verify timestamps in result are consistent
    auto table = result.value();
    if (table->num_rows() > 0) {
        auto timestamp_array =
            std::static_pointer_cast<arrow::TimestampArray>(table->column(0)->chunk(0));
        // Verify timestamps are in expected range
        EXPECT_GE(
            timestamp_array->Value(0),
            std::chrono::duration_cast<std::chrono::seconds>(ny_time.time_since_epoch()).count());
        EXPECT_LE(
            timestamp_array->Value(table->num_rows() - 1),
            std::chrono::duration_cast<std::chrono::seconds>(utc_time.time_since_epoch()).count());
    }
}
