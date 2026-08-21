#include <gtest/gtest.h>
#include <climits>
#include <stdexcept>
#include <string>
#include "trade_ngin/core/types.hpp"

using namespace trade_ngin;

class DecimalTest : public ::testing::Test {};

// ========== String parsing constructor ==========

TEST_F(DecimalTest, ParsePositiveStrings) {
    EXPECT_EQ(Decimal(std::string("1.5")), Decimal(1.5));
    EXPECT_EQ(Decimal(std::string("2.25")), Decimal(2.25));
    EXPECT_EQ(Decimal(std::string("100")), Decimal(100));
    EXPECT_EQ(Decimal(std::string("0")), Decimal(0));
    EXPECT_EQ(Decimal(std::string("0.00000001")), Decimal(0.00000001));
}

TEST_F(DecimalTest, ParseNegativeStrings) {
    // Regression: fractional digits were added instead of subtracted for
    // negatives, so "-1.5" parsed as -0.5 and "-0.5" as +0.5.
    EXPECT_EQ(Decimal(std::string("-1.5")), Decimal(-1.5));
    EXPECT_EQ(Decimal(std::string("-2.25")), Decimal(-2.25));
    EXPECT_EQ(Decimal(std::string("-100")), Decimal(-100));
}

TEST_F(DecimalTest, ParseNegativeFractionBelowOne) {
    // "-0.5" is the worst case: stoll("-0") is 0, so the old code produced
    // +0.5 - the sign flipped entirely.
    EXPECT_EQ(Decimal(std::string("-0.5")), Decimal(-0.5));
    EXPECT_EQ(Decimal(std::string("-0.00000001")), Decimal(-0.00000001));
}

TEST_F(DecimalTest, ParseTooManyDecimalsThrows) {
    EXPECT_THROW(Decimal(std::string("1.123456789")), std::overflow_error);
}

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
