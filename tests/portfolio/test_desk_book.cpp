// QT plan E5, section 3b Q2: the desk's total split back to the sleeves.
#include <gtest/gtest.h>

#include "trade_ngin/portfolio/desk_book.hpp"

using namespace trade_ngin;

namespace {
const std::map<std::string, std::set<std::string>> kUniverse = {
    {"CARRY", {"ZN", "ES"}}, {"TREND", {"ES", "CL", "ZN"}}};
}

TEST(DeskSplit, UsesTheModelsSleeveProportionsFirst) {
    const SleeveQuantities system = {{"CARRY", {{"ES", 1.0}}}, {"TREND", {{"ES", 3.0}}}};
    const SleeveQuantities previous = {{"TREND", {{"ES", 9.0}}}};
    auto r = resolve_desk_split({{"ES", 8.0}}, system, previous, kUniverse);
    ASSERT_TRUE(r.is_ok());
    const auto& w = r.value().at("ES");
    EXPECT_DOUBLE_EQ(w.at("CARRY"), 1.0);
    EXPECT_DOUBLE_EQ(w.at("TREND"), 3.0);
    const auto split = split_desk_quantity(8.0, {"CARRY", "TREND"}, w);
    EXPECT_DOUBLE_EQ(split[0], 2.0);
    EXPECT_DOUBLE_EQ(split[1], 6.0);
}

TEST(DeskSplit, FallsBackToYesterdaysBook) {
    const SleeveQuantities system = {{"TREND", {{"ES", 0.0}}}};
    const SleeveQuantities previous = {{"CARRY", {{"ES", -2.0}}}};
    auto r = resolve_desk_split({{"ES", 5.0}}, system, previous, kUniverse);
    ASSERT_TRUE(r.is_ok());
    const auto& w = r.value().at("ES");
    ASSERT_EQ(w.size(), 1u);
    EXPECT_DOUBLE_EQ(w.at("CARRY"), 2.0);  // weights are absolute quantities
    const auto split = split_desk_quantity(5.0, {"CARRY", "TREND"}, w);
    EXPECT_DOUBLE_EQ(split[0], 5.0);
    EXPECT_DOUBLE_EQ(split[1], 0.0);
}

TEST(DeskSplit, FallsBackToTheFirstSleeveWhoseUniverseHoldsTheSymbol) {
    auto r = resolve_desk_split({{"ZN", 4.0}, {"CL", -1.0}}, {}, {}, kUniverse);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().at("ZN").begin()->first, "CARRY");  // alphabetically first
    EXPECT_EQ(r.value().at("CL").begin()->first, "TREND");
}

TEST(DeskSplit, RefusesNamingASymbolNoSleeveCanTake) {
    auto r = resolve_desk_split({{"GC", 2.0}}, {}, {}, kUniverse);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("GC"), std::string::npos);
}

TEST(DeskSplit, AFlatAskOnAnUnknownSymbolNeedsNoSplit) {
    auto r = resolve_desk_split({{"GC", 0.0}}, {}, {}, kUniverse);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().count("GC"), 0u);
}

TEST(DeskSplit, SplitSumsToTheRoundedQuantity) {
    const std::map<std::string, double> w = {{"A", 1.0}, {"B", 1.0}, {"C", 1.0}};
    const auto split = split_desk_quantity(-7.0, {"A", "B", "C"}, w);
    EXPECT_DOUBLE_EQ(split[0] + split[1] + split[2], -7.0);
    const auto none = split_desk_quantity(3.0, {"A", "B"}, {});
    EXPECT_DOUBLE_EQ(none[0], 3.0);
    EXPECT_DOUBLE_EQ(none[1], 0.0);
}
