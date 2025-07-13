#include <gtest/gtest.h>
#include <memory>
#include <string>
#include "trade_ngin/core/error.hpp"

using namespace trade_ngin;

class ResultTest : public ::testing::Test {};

// Test successful Result with different types
TEST_F(ResultTest, SuccessfulResults) {
    // Test with int
    Result<int> int_result(42);
    EXPECT_TRUE(int_result.is_ok());
    EXPECT_FALSE(int_result.is_error());
    EXPECT_EQ(int_result.value(), 42);

    // Test with string
    Result<std::string> string_result("success");
    EXPECT_TRUE(string_result.is_ok());
    EXPECT_FALSE(string_result.is_error());
    EXPECT_EQ(string_result.value(), "success");

    // Test with double
    Result<double> double_result(3.14);
    EXPECT_TRUE(double_result.is_ok());
    EXPECT_FALSE(double_result.is_error());
    EXPECT_DOUBLE_EQ(double_result.value(), 3.14);
}

// Test error case
TEST_F(ResultTest, ErrorCase) {
    auto error_result =
        make_error<int>(ErrorCode::INVALID_ARGUMENT, "Test error message", "TestComponent");

    EXPECT_TRUE(error_result.is_error());
    EXPECT_FALSE(error_result.is_ok());
    EXPECT_EQ(error_result.error()->code(), ErrorCode::INVALID_ARGUMENT);

    // Use strcmp or string comparison instead of pointer comparison
    EXPECT_STREQ(error_result.error()->what(), "Test error message");
    // Or alternatively:
    // EXPECT_EQ(std::string(error_result.error()->what()), "Test error message");

    EXPECT_EQ(error_result.error()->component(), "TestComponent");
}

// Test move-only types
TEST_F(ResultTest, MoveOnlyType) {
    // Create a Result with a unique_ptr
    auto ptr = std::make_unique<int>(42);
    Result<std::unique_ptr<int>> result(std::move(ptr));

    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(result.is_error());
    EXPECT_EQ(*result.value(), 42);
}

// Test move semantics
TEST_F(ResultTest, MoveSemantics) {
    // Test with a basic type first
    Result<std::string> str_result(std::string("test"));
    Result<std::string> moved_str = std::move(str_result);

    EXPECT_TRUE(moved_str.is_ok());
    EXPECT_EQ(moved_str.value(), "test");

    // Test with unique_ptr
    auto ptr = std::make_unique<int>(42);
    Result<std::unique_ptr<int>> ptr_result(std::move(ptr));
    Result<std::unique_ptr<int>> moved_ptr = std::move(ptr_result);

    EXPECT_TRUE(moved_ptr.is_ok());
    EXPECT_EQ(*moved_ptr.value(), 42);
}

// Test void Result
TEST_F(ResultTest, VoidResult) {
    Result<void> success;
    EXPECT_TRUE(success.is_ok());
    EXPECT_FALSE(success.is_error());

    auto error = make_error<void>(ErrorCode::INVALID_ARGUMENT, "Void error", "Test");
    EXPECT_TRUE(error.is_error());
    EXPECT_FALSE(error.is_ok());
}
