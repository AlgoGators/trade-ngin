#include <gtest/gtest.h>
#include <climits>
#include <string>
#include "trade_ngin/core/types.hpp"

using namespace trade_ngin;

class DecimalTest : public ::testing::Test {};

// ========== String conversion: to_string ==========

TEST_F(DecimalTest, ToStringPositiveValues) {
    EXPECT_EQ(Decimal(1.5).to_string(), "1.5");
    EXPECT_EQ(Decimal(0.5).to_string(), "0.5");
    EXPECT_EQ(Decimal(2.25).to_string(), "2.25");
    EXPECT_EQ(Decimal(100).to_string(), "100");
    EXPECT_EQ(Decimal(0).to_string(), "0");
}

TEST_F(DecimalTest, ToStringNegativeValues) {
    EXPECT_EQ(Decimal(-1.5).to_string(), "-1.5");
    EXPECT_EQ(Decimal(-100).to_string(), "-100");
}

TEST_F(DecimalTest, ToStringNegativeFractionBelowOne) {
    // Regression: value_ / SCALE truncates -0.5 to 0, which used to drop
    // the sign and print "0.5".
    EXPECT_EQ(Decimal(-0.5).to_string(), "-0.5");
    EXPECT_EQ(Decimal(-0.00000001).to_string(), "-0.00000001");
    EXPECT_EQ(Decimal(-0.25).to_string(), "-0.25");
}

TEST_F(DecimalTest, ToStringInt64MinDoesNotOverflow) {
    // std::abs(INT64_MIN) is UB; the unsigned-magnitude path must not trip it.
    Decimal min_val = Decimal::from_raw(INT64_MIN);
    EXPECT_EQ(min_val.to_string(), "-92233720368.54775808");
}
