#include <gtest/gtest.h>
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
