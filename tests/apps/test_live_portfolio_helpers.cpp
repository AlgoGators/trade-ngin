#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/data/postgres_database.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

using namespace trade_ngin;

namespace {

Bar make_bar(const std::string& symbol, Timestamp ts, double close) {
    return Bar(ts, Price(close), Price(close), Price(close), Price(close), 1000.0, symbol);
}

Position make_position(const std::string& symbol, double qty) {
    return Position(symbol, Quantity(qty), Price(0.0), Decimal(0.0), Decimal(0.0),
                     std::chrono::system_clock::now());
}

Timestamp report_date() {
    return std::chrono::system_clock::from_time_t(1780368000);  // 2026-06-02T00:00:00Z
}

struct ReportSnapshotCall {
    std::string strategy_id;
    std::vector<std::string> strategy_names;
    std::string portfolio_id;
    Timestamp date;
    std::string portfolio_type;
};

class ReportSnapshotDatabase : public PostgresDatabase {
public:
    ReportSnapshotDatabase() : PostgresDatabase("mock://qt-report-snapshot") {}

    Result<ReportPositionRows> load_report_positions_by_date(
        const std::string& strategy_id, const std::vector<std::string>& strategy_names,
        const std::string& portfolio_id, const Timestamp& date,
        const std::string& portfolio_type) override {
        calls.push_back({strategy_id, strategy_names, portfolio_id, date, portfolio_type});
        if (return_error) {
            return make_error<ReportPositionRows>(
                ErrorCode::DATABASE_ERROR, "simulated QT read failure");
        }
        ReportPositionRows captured;
        for (const auto& name : strategy_names) captured[name] = rows[name];
        if (mutate_after_read) rows["CARRY"]["ES"] = make_position("ES", 99.0);
        return captured;
    }

    std::unordered_map<std::string, std::unordered_map<std::string, Position>> rows;
    std::vector<ReportSnapshotCall> calls;
    bool return_error{false};
    bool mutate_after_read{false};
};

class QtSeedDatabase : public PostgresDatabase {
public:
    QtSeedDatabase() : PostgresDatabase("mock://qt-seed") {}
    Result<int> seed_qt_positions_from_system(const std::string& id, const std::string& name,
        const std::string& book, const std::string& date, const std::string&) override {
        names.push_back(name);
        EXPECT_EQ(id, "LIVE_TREND_CARRY");
        EXPECT_EQ(book, "INVESTOR_A");
        EXPECT_EQ(date, "2026-06-02");
        if (fail) return make_error<int>(ErrorCode::DATABASE_ERROR, "seed failed");
        return 1;
    }
    std::vector<std::string> names;
    bool fail{false};
};

}  // namespace

// --- load_qt_report_position_snapshot ---

TEST(LivePortfolioHelpers, QtReportSeedingIncludesStrategiesWithNoSystemPositions) {
    QtSeedDatabase db;
    auto result = seed_qt_report_positions(db, "LIVE_TREND_CARRY", {"TREND", "CARRY"},
                                         "INVESTOR_A", report_date());
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(db.names, (std::vector<std::string>{"TREND", "CARRY"}));
}

TEST(LivePortfolioHelpers, QtReportSeedingPropagatesFailureToBlockReporting) {
    QtSeedDatabase db;
    db.fail = true;
    EXPECT_TRUE(seed_qt_report_positions(db, "LIVE_TREND_CARRY", {"TREND"},
                                       "INVESTOR_A", report_date()).is_error());
}

TEST(LivePortfolioHelpers, QtReportSnapshotCapturesAllStrategiesBeforeALaterWrite) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {{"ES", make_position("ES", 7.0)}};
    db.rows["CARRY"] = {{"ES", make_position("ES", -2.0)}};
    db.mutate_after_read = true;
    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND_CARRY", {"TREND", "CARRY"}, "INVESTOR_A", report_date(), {});
    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(result.value().by_strategy.at("CARRY").at("ES").quantity.as_double(), -2.0);
    EXPECT_DOUBLE_EQ(result.value().combined.at("ES").quantity.as_double(), 5.0);
    EXPECT_EQ(db.calls.size(), 1u);
}

TEST(LivePortfolioHelpers, QtReportSnapshotReplacesSystemQuantityAndFiltersClosures) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {
        {"ES", make_position("ES", 7.0)},
        {"NQ", make_position("NQ", 0.0)},
    };
    StrategyPositionRows system{{"TREND", {{"ES", make_position("ES", 12.0)}}}};

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), system);

    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(result.value().by_strategy.at("TREND").at("ES").quantity.as_double(), 7.0);
    EXPECT_EQ(result.value().by_strategy.at("TREND").count("NQ"), 0u);
    EXPECT_EQ(result.value().portfolio_id, "INVESTOR_A");
    EXPECT_EQ(result.value().strategy_id, "LIVE_TREND");
    EXPECT_EQ(result.value().portfolio_type, "qt");
    EXPECT_EQ(result.value().date, report_date());
    EXPECT_EQ(result.value().evidence_counts.at("TREND"), 2u);
}

TEST(LivePortfolioHelpers, QtReportSnapshotCopiesRemainStableForCsvAndEmailConsumers) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {{"ES", make_position("ES", 7.0)}};

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), {});

    ASSERT_TRUE(result.is_ok());
    auto csv_positions = result.value().by_strategy;
    auto email_positions = result.value().combined;

    db.rows["TREND"]["ES"] = make_position("ES", 99.0);

    EXPECT_DOUBLE_EQ(csv_positions.at("TREND").at("ES").quantity.as_double(), 7.0);
    EXPECT_DOUBLE_EQ(email_positions.at("ES").quantity.as_double(), 7.0);
}

TEST(LivePortfolioHelpers, QtReportSnapshotPropagatesScopedDatabaseErrors) {
    ReportSnapshotDatabase db;
    db.return_error = true;

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), {});

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);
    EXPECT_NE(std::string(result.error()->what()).find("INVESTOR_A"), std::string::npos);
    EXPECT_NE(std::string(result.error()->what()).find("TREND"), std::string::npos);
    EXPECT_NE(std::string(result.error()->what()).find("2026-06-02"), std::string::npos);
    EXPECT_NE(std::string(result.error()->what()).find("qt"), std::string::npos);
}

TEST(LivePortfolioHelpers, QtReportSnapshotFailsClosedForMissingSystemSymbolCoverage) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {{"ES", make_position("ES", 7.0)}};
    StrategyPositionRows system{{"TREND", {{"ES", make_position("ES", 12.0)},
                                             {"NQ", make_position("NQ", 3.0)}}}};

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), system);

    ASSERT_TRUE(result.is_error());
    EXPECT_NE(std::string(result.error()->what()).find("NQ"), std::string::npos);
}

TEST(LivePortfolioHelpers, QtReportSnapshotAcceptsAFlatStrategyWithoutRows) {
    ReportSnapshotDatabase db;

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), {});

    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().by_strategy.at("TREND").empty());
    EXPECT_TRUE(result.value().combined.empty());
}

TEST(LivePortfolioHelpers, QtReportSnapshotTreatsZeroQtRowsAsCoverageEvidence) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {{"ES", make_position("ES", 0.0)}};
    StrategyPositionRows system{{"TREND", {{"ES", make_position("ES", 12.0)}}}};

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), system);

    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().by_strategy.at("TREND").empty());
    EXPECT_TRUE(result.value().combined.empty());
}

TEST(LivePortfolioHelpers, QtReportSnapshotAggregatesDuplicateOpenSymbolsAcrossStrategies) {
    ReportSnapshotDatabase db;
    db.rows["TREND"] = {{"ES", make_position("ES", 2.0)}};
    db.rows["CARRY"] = {{"ES", make_position("ES", -0.5)}};

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND_CARRY", {"TREND", "CARRY"}, "INVESTOR_A", report_date(), {});

    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(result.value().combined.at("ES").quantity.as_double(), 1.5);
}

TEST(LivePortfolioHelpers, QtReportSnapshotUsesExactQtDatabaseScope) {
    ReportSnapshotDatabase db;

    auto result = load_qt_report_position_snapshot(
        db, "LIVE_TREND", {"TREND"}, "INVESTOR_A", report_date(), {});

    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(db.calls.size(), 1u);
    EXPECT_EQ(db.calls[0].strategy_id, "LIVE_TREND");
    EXPECT_EQ(db.calls[0].strategy_names, std::vector<std::string>{"TREND"});
    EXPECT_EQ(db.calls[0].portfolio_id, "INVESTOR_A");
    EXPECT_EQ(db.calls[0].date, report_date());
    EXPECT_EQ(db.calls[0].portfolio_type, "qt");
}

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

TEST(BuildRunInputsRowTest, OmittedStartEndDefaultToZero) {
    auto row = build_run_inputs_row("sha", {}, {}, {}, "live");
    EXPECT_EQ(row["data_window"]["start"], 0);
    EXPECT_EQ(row["data_window"]["end"], 0);
}

TEST(BuildRunInputsRowTest, StartEndPopulateWhenGiven) {
    auto start = std::chrono::system_clock::from_time_t(1000);
    auto end = std::chrono::system_clock::from_time_t(2000);
    auto row = build_run_inputs_row("sha", {}, {}, {}, "live", start, end);
    EXPECT_EQ(row["data_window"]["start"], 1000);
    EXPECT_EQ(row["data_window"]["end"], 2000);
}

// --- select_enabled_live_strategies ---

TEST(SelectEnabledLiveStrategiesTest, ErrorsOnNullOrNonObjectConfig) {
    EXPECT_TRUE(select_enabled_live_strategies(nlohmann::json()).is_error());
    EXPECT_TRUE(select_enabled_live_strategies(nlohmann::json::array()).is_error());
}

TEST(SelectEnabledLiveStrategiesTest, ErrorsWhenNoStrategyIsEnabledLive) {
    nlohmann::json cfg = {
        {"TREND_FOLLOWING", {{"enabled_live", false}, {"default_allocation", 0.5}}}};
    EXPECT_TRUE(select_enabled_live_strategies(cfg).is_error());
}

TEST(SelectEnabledLiveStrategiesTest, FiltersToEnabledLiveOnly) {
    nlohmann::json cfg = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.7}}},
        {"B", {{"enabled_live", false}, {"default_allocation", 0.3}}},
    };
    auto result = select_enabled_live_strategies(cfg);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().names, (std::vector<std::string>{"A"}));
}

TEST(SelectEnabledLiveStrategiesTest, NormalizesAllocationsToSumToOne) {
    nlohmann::json cfg = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.6}}},
        {"B", {{"enabled_live", true}, {"default_allocation", 0.3}}},
    };
    auto result = select_enabled_live_strategies(cfg);
    ASSERT_TRUE(result.is_ok());
    const auto& selection = result.value();
    EXPECT_DOUBLE_EQ(selection.allocation_sum_before_normalization, 0.9);
    EXPECT_NEAR(selection.allocations.at("A") + selection.allocations.at("B"), 1.0, 1e-9);
    EXPECT_NEAR(selection.allocations.at("A") / selection.allocations.at("B"), 0.6 / 0.3, 1e-9);
}

TEST(SelectEnabledLiveStrategiesTest, NamesAreSortedForDeterministicCombinedId) {
    nlohmann::json cfg = {
        {"ZEBRA", {{"enabled_live", true}, {"default_allocation", 0.5}}},
        {"ALPHA", {{"enabled_live", true}, {"default_allocation", 0.5}}},
    };
    auto result = select_enabled_live_strategies(cfg);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().names, (std::vector<std::string>{"ALPHA", "ZEBRA"}));
}

TEST(SelectEnabledLiveStrategiesTest, MissingDefaultAllocationDefaultsToHalf) {
    nlohmann::json cfg = {{"A", {{"enabled_live", true}}}};
    auto result = select_enabled_live_strategies(cfg);
    ASSERT_TRUE(result.is_ok());
    // Single strategy, no default_allocation given (defaults to 0.5) -- normalizes to 1.0
    // regardless, but allocation_sum_before_normalization should reflect the 0.5 default.
    EXPECT_DOUBLE_EQ(result.value().allocation_sum_before_normalization, 0.5);
    EXPECT_DOUBLE_EQ(result.value().allocations.at("A"), 1.0);
}

// --- build_combined_strategy_id ---

TEST(BuildCombinedStrategyIdTest, JoinsSortedNamesWithLivePrefix) {
    EXPECT_EQ(build_combined_strategy_id({"ALPHA", "ZEBRA"}), "LIVE_ALPHA_ZEBRA");
}

TEST(BuildCombinedStrategyIdTest, SingleNameNoTrailingUnderscore) {
    EXPECT_EQ(build_combined_strategy_id({"ALPHA"}), "LIVE_ALPHA");
}

TEST(BuildCombinedStrategyIdTest, EmptyNamesProducesBarePrefix) {
    EXPECT_EQ(build_combined_strategy_id({}), "LIVE_");
}

// --- group_into_sha_batches ---

TEST(GroupIntoShaBatchesTest, EmptyInputProducesEmptyOutput) {
    EXPECT_TRUE(group_into_sha_batches({}).empty());
}

TEST(GroupIntoShaBatchesTest, SingleShaCollapsesToOneBatch) {
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"2026-08-01", "aaa"}, {"2026-08-02", "aaa"}, {"2026-08-03", "aaa"}};
    auto batches = group_into_sha_batches(pairs);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].sha, "aaa");
    EXPECT_EQ(batches[0].from_date, "2026-08-01");
    EXPECT_EQ(batches[0].through_date, "2026-08-03");
}

TEST(GroupIntoShaBatchesTest, ShaChangeStartsNewBatch) {
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"2026-08-01", "aaa"}, {"2026-08-02", "aaa"}, {"2026-08-03", "bbb"}};
    auto batches = group_into_sha_batches(pairs);
    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0].sha, "aaa");
    EXPECT_EQ(batches[0].from_date, "2026-08-01");
    EXPECT_EQ(batches[0].through_date, "2026-08-02");
    EXPECT_EQ(batches[1].sha, "bbb");
    EXPECT_EQ(batches[1].from_date, "2026-08-03");
    EXPECT_EQ(batches[1].through_date, "2026-08-03");
}

TEST(GroupIntoShaBatchesTest, ShaRevertingLaterStartsANewBatchNotReopened) {
    // aaa -> bbb -> aaa again: the second "aaa" run must be its own batch,
    // not merged back into the first (they are not contiguous).
    std::vector<std::pair<std::string, std::string>> pairs = {
        {"2026-08-01", "aaa"}, {"2026-08-02", "bbb"}, {"2026-08-03", "aaa"}};
    auto batches = group_into_sha_batches(pairs);
    ASSERT_EQ(batches.size(), 3u);
    EXPECT_EQ(batches[0].sha, "aaa");
    EXPECT_EQ(batches[2].sha, "aaa");
    EXPECT_EQ(batches[0].from_date, "2026-08-01");
    EXPECT_EQ(batches[2].from_date, "2026-08-03");
}

TEST(GroupIntoShaBatchesTest, SingleRowProducesSingleBatch) {
    std::vector<std::pair<std::string, std::string>> pairs = {{"2026-08-01", "aaa"}};
    auto batches = group_into_sha_batches(pairs);
    ASSERT_EQ(batches.size(), 1u);
    EXPECT_EQ(batches[0].from_date, batches[0].through_date);
}
