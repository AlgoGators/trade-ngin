#include <gtest/gtest.h>
#include "trade_ngin/core/error.hpp"

using namespace trade_ngin;

// A simple test to verify our testing setup works
TEST(ResultTest, SimpleTest) {
    Result<int> success_result(42);
    EXPECT_TRUE(success_result.is_ok());
    EXPECT_EQ(success_result.value(), 42);
}