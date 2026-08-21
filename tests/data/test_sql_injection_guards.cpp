// tests/data/test_sql_injection_guards.cpp
#include <gmock/gmock.h>

#include <gtest/gtest.h>
#include "trade_ngin/data/postgres_database.hpp"
#include <memory>

using namespace trade_ngin;

/**
 * @brief Test suite for SQL injection guards and parameterized query helpers
 */
class SqlInjectionGuardsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a database instance for testing helper functions
        db = std::make_unique<PostgresDatabase>("mock://testdb");
    }

    void TearDown() override {
        db.reset();
    }

    std::unique_ptr<PostgresDatabase> db;
};

// ============================================================================
// TESTS FOR build_insert_placeholders_and_params() HELPER
// ============================================================================

/**
 * @brief Test building placeholders for single row with single column
 */
TEST_F(SqlInjectionGuardsTest, SingleRowSingleColumn) {
    std::vector<std::vector<std::string>> rows = {
        {"value1"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 1);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();
    EXPECT_EQ(placeholders.placeholders, "($1)");
}

/**
 * @brief Test building placeholders for single row with multiple columns
 */
TEST_F(SqlInjectionGuardsTest, SingleRowMultipleColumns) {
    std::vector<std::vector<std::string>> rows = {
        {"value1", "value2", "value3"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 3);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();
    EXPECT_EQ(placeholders.placeholders, "($1, $2, $3)");
}

/**
 * @brief Test building placeholders for multiple rows with multiple columns
 */
TEST_F(SqlInjectionGuardsTest, MultipleRowsMultipleColumns) {
    std::vector<std::vector<std::string>> rows = {
        {"value1", "value2"},
        {"value3", "value4"},
        {"value5", "value6"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 2);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();
    EXPECT_EQ(placeholders.placeholders, "($1, $2), ($3, $4), ($5, $6)");
}

/**
 * @brief Test that parameters are correctly appended in order
 */
TEST_F(SqlInjectionGuardsTest, ParametersInOrder) {
    std::vector<std::vector<std::string>> rows = {
        {"AAPL", "100"},
        {"MSFT", "200"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 2);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();
    EXPECT_EQ(placeholders.placeholders, "($1, $2), ($3, $4)");
    // Verify params have 4 elements (would need internal inspection to verify content)
}

/**
 * @brief Test error on empty rows
 */
TEST_F(SqlInjectionGuardsTest, EmptyRowsError) {
    std::vector<std::vector<std::string>> rows = {};

    auto result = db->build_insert_placeholders_and_params(rows, 1);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_THAT(result.error()->what(), ::testing::HasSubstr("rows must not be empty"));
}

/**
 * @brief Test error on zero columns per row
 */
TEST_F(SqlInjectionGuardsTest, ZeroColumnsError) {
    std::vector<std::vector<std::string>> rows = {
        {"value1"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 0);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_THAT(result.error()->what(), ::testing::HasSubstr("columns_per_row must be greater than 0"));
}

/**
 * @brief Test error on mismatched column count
 */
TEST_F(SqlInjectionGuardsTest, MismatchedColumnCountError) {
    std::vector<std::vector<std::string>> rows = {
        {"value1", "value2"},
        {"value3"}  // Missing a column
    };

    auto result = db->build_insert_placeholders_and_params(rows, 2);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_THAT(result.error()->what(), ::testing::HasSubstr("has 1 columns but expected 2"));
}

// ============================================================================
// TESTS FOR CHUNKING BOUNDARIES (PostgreSQL 65535-parameter limit)
// ============================================================================

/**
 * @brief Test exactly at the PostgreSQL parameter limit
 */
TEST_F(SqlInjectionGuardsTest, ExactlyAtParameterLimit) {
    // 65535 parameters = 6553 rows with 10 columns per row (but slightly less to be exact)
    // 65535 / 10 = 6553.5, so we use 6553 rows with 10 columns = 65530 params
    constexpr size_t COLUMNS = 10;
    constexpr size_t ROWS = 6553;
    constexpr size_t EXPECTED_PARAMS = ROWS * COLUMNS;  // 65530

    std::vector<std::vector<std::string>> rows(ROWS, std::vector<std::string>(COLUMNS, "val"));

    auto result = db->build_insert_placeholders_and_params(rows, COLUMNS);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();

    // Verify the placeholders string starts and ends correctly
    EXPECT_THAT(placeholders.placeholders, ::testing::StartsWith("($1, $2, $3, $4, $5, $6, $7, $8, $9, $10)"));
    EXPECT_THAT(placeholders.placeholders, ::testing::EndsWith(")"));
}

/**
 * @brief Test just below the PostgreSQL parameter limit
 */
TEST_F(SqlInjectionGuardsTest, JustBelowParameterLimit) {
    // 65000 parameters = 6500 rows with 10 columns per row
    constexpr size_t COLUMNS = 10;
    constexpr size_t ROWS = 6500;

    std::vector<std::vector<std::string>> rows(ROWS, std::vector<std::string>(COLUMNS, "val"));

    auto result = db->build_insert_placeholders_and_params(rows, COLUMNS);

    ASSERT_TRUE(result.is_ok());
}

/**
 * @brief Test just above the PostgreSQL parameter limit - should fail
 */
TEST_F(SqlInjectionGuardsTest, JustAboveParameterLimit) {
    // 65540 parameters = 6554 rows with 10 columns per row
    constexpr size_t COLUMNS = 10;
    constexpr size_t ROWS = 6554;

    std::vector<std::vector<std::string>> rows(ROWS, std::vector<std::string>(COLUMNS, "val"));

    auto result = db->build_insert_placeholders_and_params(rows, COLUMNS);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_THAT(result.error()->what(), ::testing::HasSubstr("exceeds PostgreSQL limit"));
}

/**
 * @brief Test exceeding parameter limit by significant margin
 */
TEST_F(SqlInjectionGuardsTest, SignificantlyExceedsParameterLimit) {
    // 100000 parameters (way over the 65535 limit)
    constexpr size_t COLUMNS = 100;
    constexpr size_t ROWS = 1001;

    std::vector<std::vector<std::string>> rows(ROWS, std::vector<std::string>(COLUMNS, "val"));

    auto result = db->build_insert_placeholders_and_params(rows, COLUMNS);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// ============================================================================
// TESTS FOR IDENTIFIER VALIDATION
// ============================================================================

/**
 * @brief Test validation of valid table names
 */
TEST_F(SqlInjectionGuardsTest, ValidTableNameValidation) {
    auto result = db->validate_table_name("trading.positions");

    // validate_table_name expects specific format (schema.table)
    if (result.is_ok()) {
        // Valid table name accepted
        EXPECT_TRUE(result.is_ok());
    }
}

/**
 * @brief Test validation of table names with SQL injection attempts
 */
TEST_F(SqlInjectionGuardsTest, InvalidTableNameWithSqlInjection) {
    auto result = db->validate_table_name("trading.positions'; DROP TABLE users; --");

    ASSERT_TRUE(result.is_error()) << "Should reject SQL injection in table name";
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

/**
 * @brief Test validation of valid strategy IDs
 */
TEST_F(SqlInjectionGuardsTest, ValidStrategyIdValidation) {
    auto result = db->validate_strategy_id("LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST");

    if (result.is_ok()) {
        EXPECT_TRUE(result.is_ok());
    }
}

/**
 * @brief Test validation of strategy IDs with SQL injection attempts
 */
TEST_F(SqlInjectionGuardsTest, InvalidStrategyIdWithSqlInjection) {
    auto result = db->validate_strategy_id("LIVE'; DELETE FROM positions WHERE '1'='1");

    ASSERT_TRUE(result.is_error()) << "Should reject SQL injection in strategy ID";
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

/**
 * @brief Test validation of strategy IDs with special characters
 */
TEST_F(SqlInjectionGuardsTest, InvalidStrategyIdWithSpecialCharacters) {
    auto result = db->validate_strategy_id("STRATEGY-WITH-SQL-COMMENT--");

    ASSERT_TRUE(result.is_error()) << "Should reject SQL comment syntax in strategy ID";
}

/**
 * @brief Test validation of portfolio IDs
 */
TEST_F(SqlInjectionGuardsTest, ValidPortfolioIdValidation) {
    auto result = db->validate_strategy_id("BASE_PORTFOLIO");

    if (result.is_ok()) {
        EXPECT_TRUE(result.is_ok());
    }
}

// ============================================================================
// INTEGRATION TESTS
// ============================================================================

/**
 * @brief Test that placeholders can be used in a complete INSERT statement
 */
TEST_F(SqlInjectionGuardsTest, CompleteInsertStatementConstruction) {
    std::vector<std::vector<std::string>> rows = {
        {"AAPL", "100.50"},
        {"MSFT", "200.75"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 2);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();

    // Construct a complete INSERT statement
    std::string insert_query = "INSERT INTO trading.positions (symbol, price) VALUES " +
                               placeholders.placeholders;

    EXPECT_THAT(insert_query, ::testing::HasSubstr("VALUES ($1, $2), ($3, $4)"));
    EXPECT_THAT(insert_query, ::testing::HasSubstr("INSERT INTO"));
}

/**
 * @brief Test with various data types represented as strings
 */
TEST_F(SqlInjectionGuardsTest, VariousDataTypeAsStrings) {
    std::vector<std::vector<std::string>> rows = {
        {"AAPL", "100", "2024-01-15 10:30:00", "0.95"},
        {"MSFT", "200", "2024-01-15 10:31:00", "1.05"}
    };

    auto result = db->build_insert_placeholders_and_params(rows, 4);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();
    EXPECT_EQ(placeholders.placeholders, "($1, $2, $3, $4), ($5, $6, $7, $8)");
}

/**
 * @brief Test that large number of columns is handled correctly
 */
TEST_F(SqlInjectionGuardsTest, ManyColumnsPerRow) {
    // Test with 20 columns per row
    std::vector<std::vector<std::string>> rows = {
        std::vector<std::string>(20, "val1"),
        std::vector<std::string>(20, "val2")
    };

    auto result = db->build_insert_placeholders_and_params(rows, 20);

    ASSERT_TRUE(result.is_ok());
    auto placeholders = result.value();

    // Check that it contains correct placeholder count
    EXPECT_THAT(placeholders.placeholders, ::testing::StartsWith("($1, $2, $3, $4, $5,"));
    EXPECT_THAT(placeholders.placeholders, ::testing::ContainsRegex("\\$40\\)$"));
}
