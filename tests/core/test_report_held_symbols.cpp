// tests/core/test_report_held_symbols.cpp
//
// The Symbols Reference lists a contract the sleeves hold in opposite directions
// (BASE 2026-04-30 and 05-01, MBT), whose book entry is a zero row.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/report_held_symbols.hpp"

using namespace trade_ngin;

namespace {
Position at(const std::string& symbol, double quantity) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(quantity);
    return p;
}
}  // namespace

TEST(ReportHeldSymbols, ANetFlatContractTheSleevesHoldIsListed) {
    const std::unordered_map<std::string, Position> book{{"ES.v.0", at("ES.v.0", 1)},
                                                          {"MBT.v.0", at("MBT.v.0", 0)}};
    const StrategyPositionsMap sleeves{{"FAST", {{"MBT.v.0", at("MBT.v.0", -1)}}},
                                       {"SLOW", {{"MBT.v.0", at("MBT.v.0", 1)}}}};
    const auto held = held_by_any_sleeve(book, sleeves);
    ASSERT_EQ(held.count("MBT.v.0"), 1u);
    EXPECT_NE(held.at("MBT.v.0").quantity.as_double(), 0.0);
    EXPECT_EQ(held.at("ES.v.0").quantity.as_double(), 1.0);
}

TEST(ReportHeldSymbols, AContractNoSleeveHoldsIsNotAdded) {
    const std::unordered_map<std::string, Position> book{{"ES.v.0", at("ES.v.0", 1)}};
    const StrategyPositionsMap sleeves{{"FAST", {{"ZN.v.0", at("ZN.v.0", 0)}}}};
    const auto held = held_by_any_sleeve(book, sleeves);
    EXPECT_EQ(held.count("ZN.v.0"), 0u);
    EXPECT_EQ(held.size(), 1u);
}

TEST(ReportHeldSymbols, TheBooksOwnNonZeroEntryIsKept) {
    const std::unordered_map<std::string, Position> book{{"ES.v.0", at("ES.v.0", 2)}};
    const StrategyPositionsMap sleeves{{"FAST", {{"ES.v.0", at("ES.v.0", 1)}}}};
    EXPECT_EQ(held_by_any_sleeve(book, sleeves).at("ES.v.0").quantity.as_double(), 2.0);
}
