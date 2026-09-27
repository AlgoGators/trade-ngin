#include "trade_ngin/portfolio/quantity_rule.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace trade_ngin {
namespace {

using Mode = QuantityRoundingMode;

Quantity raw(int64_t value) { return Quantity::from_raw(value); }

QuantityRule rule(int64_t increment, Mode mode, int64_t minimum,
                  int64_t maximum) {
    return {raw(increment), mode, raw(minimum), raw(maximum)};
}

void expect_raw(int64_t value, const QuantityRule& input_rule, int64_t expected) {
    const auto result = normalize_quantity(raw(value), input_rule);
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    EXPECT_EQ(result.value().raw_value(), expected);
}

void expect_rejected(int64_t value, const QuantityRule& input_rule) {
    const auto result = normalize_quantity(raw(value), input_rule);
    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST(QuantityRuleTest, RejectModePreservesOnlyExactPositiveNegativeAndZero) {
    const auto input_rule = rule(25, Mode::reject_off_increment, -100, 100);
    expect_raw(50, input_rule, 50);
    expect_raw(-75, input_rule, -75);
    expect_raw(0, input_rule, 0);
    expect_rejected(49, input_rule);
    expect_rejected(-74, input_rule);
}

TEST(QuantityRuleTest, TowardZeroTruncatesBothSignsWithOddAndFractionalSteps) {
    const auto odd = rule(3, Mode::toward_zero, -20, 20);
    expect_raw(8, odd, 6);
    expect_raw(-8, odd, -6);
    expect_raw(9, odd, 9);
    expect_raw(-9, odd, -9);
    expect_raw(0, odd, 0);

    // Raw step 25 is 0.00000025 quantity, so fractional steps stay exact.
    const auto fractional = rule(25, Mode::toward_zero, -100, 100);
    expect_raw(74, fractional, 50);
    expect_raw(-74, fractional, -50);
}

TEST(QuantityRuleTest, NearestRoundsMidpointsAwayAndAdjacentTicksToClosest) {
    const auto even = rule(10, Mode::nearest_ties_away_from_zero, -100, 100);
    expect_raw(4, even, 0);
    expect_raw(5, even, 10);
    expect_raw(6, even, 10);
    expect_raw(-4, even, 0);
    expect_raw(-5, even, -10);
    expect_raw(-6, even, -10);
    expect_raw(20, even, 20);
    expect_raw(-20, even, -20);
    expect_raw(0, even, 0);

    const auto odd = rule(3, Mode::nearest_ties_away_from_zero, -20, 20);
    expect_raw(1, odd, 0);
    expect_raw(2, odd, 3);
    expect_raw(-1, odd, 0);
    expect_raw(-2, odd, -3);
}

TEST(QuantityRuleTest, MissingAndUnknownModesReject) {
    expect_rejected(0, QuantityRule{});
    expect_rejected(5, rule(1, Mode::unspecified, -10, 10));
    expect_rejected(5, rule(1, static_cast<Mode>(99), -10, 10));
}

TEST(QuantityRuleTest, NonpositiveIncrementAndMissingOrReversedBoundsReject) {
    expect_rejected(0, rule(0, Mode::toward_zero, -10, 10));
    expect_rejected(0, rule(-1, Mode::toward_zero, -10, 10));
    expect_rejected(0, rule(1, Mode::toward_zero, 1, -1));
    expect_rejected(0, {raw(1), Mode::toward_zero, std::nullopt, raw(10)});
    expect_rejected(0, {raw(1), Mode::toward_zero, raw(-10), std::nullopt});
}

TEST(QuantityRuleTest, SubmittedQuantityOutsideInclusiveBoundsRejects) {
    const auto input_rule = rule(2, Mode::toward_zero, -5, 5);
    expect_raw(-5, input_rule, -4);
    expect_raw(5, input_rule, 4);
    expect_rejected(-6, input_rule);
    expect_rejected(6, input_rule);
    expect_rejected(0, rule(1, Mode::reject_off_increment, 1, 10));
}

TEST(QuantityRuleTest, NormalizedQuantityOutsideBoundsRejectsWithoutClamping) {
    expect_rejected(1, rule(4, Mode::toward_zero, 1, 9));
    expect_rejected(-1, rule(4, Mode::toward_zero, -9, -1));
    expect_rejected(10, rule(4, Mode::nearest_ties_away_from_zero, 7, 10));
    expect_rejected(-10, rule(4, Mode::nearest_ties_away_from_zero, -10, -7));
}

TEST(QuantityRuleTest, ExactSignedLimitsAndLargeIncrementsAreSafe) {
    constexpr auto low = std::numeric_limits<int64_t>::min();
    constexpr auto high = std::numeric_limits<int64_t>::max();
    expect_raw(low, rule(1, Mode::reject_off_increment, low, high), low);
    expect_raw(high, rule(1, Mode::reject_off_increment, low, high), high);
    expect_raw(low, rule(2, Mode::toward_zero, low, high), low);
    expect_raw(high, rule(high, Mode::toward_zero, low, high), high);
    expect_raw(low, rule(high, Mode::toward_zero, low, high), -high);
    expect_raw(low, rule(high, Mode::nearest_ties_away_from_zero, low, high),
               -high);
}

TEST(QuantityRuleTest, UnrepresentableRoundedResultsReturnErrors) {
    constexpr auto low = std::numeric_limits<int64_t>::min();
    constexpr auto high = std::numeric_limits<int64_t>::max();
    expect_rejected(high, rule(2, Mode::nearest_ties_away_from_zero, low, high));
    expect_rejected(low, rule(3, Mode::nearest_ties_away_from_zero, low, high));
}

TEST(QuantityRuleTest, RepeatedCallsAreDeterministicAndSecondPassIsIdempotent) {
    const auto input_rule = rule(10, Mode::nearest_ties_away_from_zero, -100, 100);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto first = normalize_quantity(raw(-35), input_rule);
        ASSERT_TRUE(first.is_ok());
        EXPECT_EQ(first.value().raw_value(), -40);
        const auto second = normalize_quantity(first.value(), input_rule);
        ASSERT_TRUE(second.is_ok());
        EXPECT_EQ(second.value().raw_value(), -40);
    }
}

}  // namespace
}  // namespace trade_ngin
