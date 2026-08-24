#include "trade_ngin/apps/live_portfolio_helpers.hpp"

#include <gtest/gtest.h>

using namespace trade_ngin;

namespace {

Bar make_bar(const std::string& symbol, Timestamp ts, double close) {
    return Bar(ts, Price(close), Price(close), Price(close), Price(close), 1000.0, symbol);
}

Position make_position(const std::string& symbol, double qty) {
    return Position(symbol, Quantity(qty), Price(0.0), Decimal(0.0), Decimal(0.0),
                     std::chrono::system_clock::now());
}

}  // namespace

// --- latest_bar_by_symbol ---

TEST(LatestBarBySymbolTest, EmptyInputReturnsEmptyMap) {
    EXPECT_TRUE(latest_bar_by_symbol({}).empty());
}

TEST(LatestBarBySymbolTest, LaterBarForSameSymbolWins) {
    auto t1 = std::chrono::system_clock::now();
    auto t2 = t1 + std::chrono::hours(24);
    std::vector<Bar> bars = {make_bar("ES", t1, 100.0), make_bar("ES", t2, 105.0)};

    auto latest = latest_bar_by_symbol(bars);

    ASSERT_EQ(latest.size(), 1u);
    EXPECT_DOUBLE_EQ(latest.at("ES").close.as_double(), 105.0);
}

TEST(LatestBarBySymbolTest, KeepsOneEntryPerDistinctSymbol) {
    auto t1 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {make_bar("ES", t1, 100.0), make_bar("NQ", t1, 200.0)};

    auto latest = latest_bar_by_symbol(bars);

    ASSERT_EQ(latest.size(), 2u);
    EXPECT_DOUBLE_EQ(latest.at("ES").close.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(latest.at("NQ").close.as_double(), 200.0);
}

// --- compute_mark_to_market_equity ---

TEST(ComputeMarkToMarketEquityTest, SumsQuantityTimesLatestClose) {
    std::unordered_map<std::string, Position> positions = {
        {"ES", make_position("ES", 2.0)}, {"NQ", make_position("NQ", -1.0)}};
    std::unordered_map<std::string, Bar> latest_bars = {
        {"ES", make_bar("ES", std::chrono::system_clock::now(), 100.0)},
        {"NQ", make_bar("NQ", std::chrono::system_clock::now(), 50.0)}};

    // 2 * 100 + (-1) * 50 = 150
    EXPECT_DOUBLE_EQ(compute_mark_to_market_equity(positions, latest_bars), 150.0);
}

TEST(ComputeMarkToMarketEquityTest, PositionWithNoMatchingBarIsSkippedNotZeroed) {
    std::unordered_map<std::string, Position> positions = {{"ES", make_position("ES", 2.0)},
                                                             {"ZZZ", make_position("ZZZ", 5.0)}};
    std::unordered_map<std::string, Bar> latest_bars = {
        {"ES", make_bar("ES", std::chrono::system_clock::now(), 100.0)}};

    // ZZZ has no bar; only ES contributes (2 * 100 = 200), not ES + 0 for ZZZ.
    EXPECT_DOUBLE_EQ(compute_mark_to_market_equity(positions, latest_bars), 200.0);
}

TEST(ComputeMarkToMarketEquityTest, EmptyPositionsReturnZero) {
    EXPECT_DOUBLE_EQ(compute_mark_to_market_equity({}, {}), 0.0);
}

// --- build_run_inputs_row ---

TEST(BuildRunInputsRowTest, PopulatesTopLevelFieldsVerbatim) {
    auto row = build_run_inputs_row("abc123", nlohmann::json{{"key", "value"}}, {"ES", "NQ"}, {},
                                     "live");

    EXPECT_EQ(row["trade_ngin_sha"], "abc123");
    EXPECT_EQ(row["config_snapshot"]["key"], "value");
    EXPECT_EQ(row["universe"], (std::vector<std::string>{"ES", "NQ"}));
    EXPECT_EQ(row["engine_flags"]["benchmark_mode"], "live");
    EXPECT_TRUE(row["risk_limits_id"].is_null());
    EXPECT_TRUE(row["engine_flags"]["rng_seed"].is_null());
}

TEST(BuildRunInputsRowTest, ContentHashIsSensitiveToBarDataChanges) {
    auto t1 = std::chrono::system_clock::now();
    std::vector<Bar> bars_a = {make_bar("ES", t1, 100.0)};
    std::vector<Bar> bars_b = {make_bar("ES", t1, 100.5)};  // one price differs

    auto row_a = build_run_inputs_row("sha", {}, {}, bars_a, "live");
    auto row_b = build_run_inputs_row("sha", {}, {}, bars_b, "live");

    EXPECT_NE(row_a["data_window"]["content_hash"], row_b["data_window"]["content_hash"]);
}

TEST(BuildRunInputsRowTest, ContentHashIsStableForIdenticalBars) {
    auto t1 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {make_bar("ES", t1, 100.0), make_bar("NQ", t1, 200.0)};

    auto row_1 = build_run_inputs_row("sha", {}, {}, bars, "live");
    auto row_2 = build_run_inputs_row("sha", {}, {}, bars, "live");

    EXPECT_EQ(row_1["data_window"]["content_hash"], row_2["data_window"]["content_hash"]);
    EXPECT_EQ(row_1["data_window"]["row_count"], 2);
}

TEST(BuildRunInputsRowTest, EmptyBarsProducesRowCountZeroNotAFailure) {
    auto row = build_run_inputs_row("sha", {}, {}, {}, "deferred");
    EXPECT_EQ(row["data_window"]["row_count"], 0);
    EXPECT_FALSE(row["data_window"]["content_hash"].get<std::string>().empty());
}
