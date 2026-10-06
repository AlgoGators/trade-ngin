#include "trade_ngin/data/postgres_database.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

// Keep the existing test-only private-access pattern local to these declarations.
// It observes what the original factory actually passed through construction.
#define private public
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"
#undef private

#include "trade_ngin/apps/live_portfolio_helpers.hpp"
#include "trade_ngin/apps/live_runtime_invocation.hpp"

#include <gtest/gtest.h>

#include <string>
#include <stdexcept>
#include <limits>
#include <typeinfo>
#include <unordered_map>
#include <vector>

using namespace trade_ngin;

TEST(PortfolioSelectionTest, DefaultsAndPreservesDateAndEmailArguments) {
    auto selected = resolve_portfolio_selection(
        {"2026-09-30", "--send-email"}, std::nullopt, "base");
    ASSERT_TRUE(selected.is_ok());
    EXPECT_EQ(selected.value().config_name, "base");
    EXPECT_EQ(selected.value().runner_arguments,
              (std::vector<std::string>{"2026-09-30", "--send-email"}));
}

TEST(PortfolioSelectionTest, CliOverridesEnvironmentAndKeepsGovernedPriorArguments) {
    const std::vector<std::string> arguments = {
        "--verified-desk-prior", "--prior-decision", "a0000000-0000-4000-8000-000000000001",
        "--portfolio", "investor-7", "--prior-finalization",
        "b0000000-0000-4000-8000-000000000001", "2026-09-30"};
    auto selected = resolve_portfolio_selection(arguments, "equity_mr", "base");
    ASSERT_TRUE(selected.is_ok());
    EXPECT_EQ(selected.value().config_name, "investor-7");
    EXPECT_EQ(selected.value().runner_arguments,
              (std::vector<std::string>{"--verified-desk-prior", "--prior-decision",
                  "a0000000-0000-4000-8000-000000000001", "--prior-finalization",
                  "b0000000-0000-4000-8000-000000000001", "2026-09-30"}));
}

TEST(PortfolioSelectionTest, EnvironmentOverridesDefault) {
    auto selected = resolve_portfolio_selection({}, "investor_a", "base");
    ASSERT_TRUE(selected.is_ok());
    EXPECT_EQ(selected.value().config_name, "investor_a");
    EXPECT_TRUE(selected.value().runner_arguments.empty());
}

TEST(PortfolioSelectionTest, DuplicateMissingAndUnknownFlagsFailClosed) {
    EXPECT_TRUE(resolve_portfolio_selection(
        {"--portfolio", "base", "--portfolio", "base"}, std::nullopt, "base").is_error());
    EXPECT_TRUE(resolve_portfolio_selection(
        {"--portfolio"}, std::nullopt, "base").is_error());
    EXPECT_TRUE(resolve_portfolio_selection(
        {"--send-emali"}, std::nullopt, "base").is_error());
}

TEST(PortfolioSelectionTest, UnsafeAndOversizedKeysFailClosedAtEveryPrecedenceLevel) {
    const std::vector<std::string> invalid = {
        "", "/tmp/book", "../book", "book/name", "book.name", "UPPER", "_hidden",
        std::string(65, 'a')};
    for (const auto& key : invalid) {
        EXPECT_TRUE(resolve_portfolio_selection(
            {"--portfolio", key}, std::nullopt, "base").is_error()) << key;
        EXPECT_TRUE(resolve_portfolio_selection({}, key, "base").is_error()) << key;
        EXPECT_TRUE(resolve_portfolio_selection({}, std::nullopt, key).is_error()) << key;
    }
    auto boundary = resolve_portfolio_selection(
        {"--portfolio", "a" + std::string(63, '-')}, std::nullopt, "base");
    ASSERT_TRUE(boundary.is_ok());
    EXPECT_EQ(boundary.value().config_name.size(), 64u);
}

TEST(LiveRuntimeControl, ControlledSelectionRefusesCapitalRedistribution) {
    nlohmann::json configured = {
        {"TREND", {{"enabled_live", true}, {"default_allocation", 0.7}}},
        {"FAST", {{"enabled_live", false}, {"default_allocation", 0.3}}}};
    EXPECT_TRUE(select_controlled_live_strategies(configured).is_error());
    configured["FAST"]["enabled_live"] = true;
    auto selected = select_controlled_live_strategies(configured);
    ASSERT_TRUE(selected.is_ok());
    EXPECT_DOUBLE_EQ(selected.value().allocations.at("TREND"), 0.7);
    EXPECT_DOUBLE_EQ(selected.value().allocations.at("FAST"), 0.3);
}

TEST(LiveRuntimeControl, SnapshotOmitsTransportCredentialsAndRejectsNestedSecrets) {
    AppConfig config;
    config.portfolio_id = "BOOK";
    config.database.password = "synthetic-db-secret";
    config.email.password = "synthetic-email-secret";
    config.strategies_config = {{"TREND", {{"enabled_live", true}, {"default_allocation", 1.0}}}};
    auto snapshot = build_runtime_trading_snapshot(config);
    ASSERT_TRUE(snapshot.is_ok());
    EXPECT_EQ(snapshot.value().at("snapshot_version"), 2);
    EXPECT_TRUE(snapshot.value().contains("sleeve_risk_modules"));
    EXPECT_EQ(snapshot.value().at("risk").at("schema"), 2);
    EXPECT_EQ(snapshot.value().at("initial_capital"), 500000.0);
    EXPECT_FALSE(snapshot.value().contains("email"));
    EXPECT_FALSE(snapshot.value().contains("database"));
    EXPECT_EQ(snapshot.value().dump().find("synthetic-"), std::string::npos);
    config.strategies_config["TREND"]["config"]["apiToken"] = "synthetic-nested-secret";
    EXPECT_TRUE(build_runtime_trading_snapshot(config).is_error());
}

TEST(LiveRuntimeControl, ActivationRequiresExactExplicitTrue) {
    EXPECT_FALSE(runtime_control_enabled(nullptr));
    EXPECT_FALSE(runtime_control_enabled(""));
    EXPECT_FALSE(runtime_control_enabled("TRUE"));
    EXPECT_FALSE(runtime_control_enabled("1"));
    EXPECT_FALSE(runtime_control_enabled(" true "));
    EXPECT_TRUE(runtime_control_enabled("true"));
}

TEST(LiveRuntimeControl, SchedulerExplicitDateDoesNotBypassControl) {
    const auto explicit_date = std::chrono::system_clock::from_time_t(1790035200);
    auto decision = resolve_live_runtime_control("true", explicit_date,
        explicit_date + std::chrono::hours(18));
    ASSERT_TRUE(decision.is_ok());
    EXPECT_TRUE(decision.value());
    EXPECT_TRUE(resolve_live_runtime_control("true",explicit_date-std::chrono::hours(24),
        explicit_date).is_error());
    auto disabled = resolve_live_runtime_control(nullptr,explicit_date-std::chrono::hours(24),explicit_date);
    ASSERT_TRUE(disabled.is_ok());
    EXPECT_FALSE(disabled.value());
}

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

struct ProposalSeedCall {
    std::string strategy_id;
    std::string strategy_name;
    std::string portfolio_id;
    std::string date;
};

class ProposalSeedDatabase : public PostgresDatabase {
public:
    ProposalSeedDatabase() : PostgresDatabase("mock://qt-proposal-seed") {}

    Result<int> seed_qt_proposal_positions_from_system(
        const std::string& id, const std::string& name,
        const std::string& book, const std::string& date) override {
        calls.push_back({id, name, book, date});
        if (name == failing_name) {
            return make_error<int>(ErrorCode::DATABASE_ERROR, "synthetic proposal seed failure");
        }
        return 0;  // A queued zero is successful, including for a flat component.
    }

    Result<int> seed_qt_positions_from_system(
        const std::string&, const std::string&, const std::string&,
        const std::string&, const std::string&) override {
        ++legacy_seed_calls;
        return 1;
    }

    std::vector<ProposalSeedCall> calls;
    std::string failing_name;
    int legacy_seed_calls{0};
};

}  // namespace

// --- load_qt_report_position_snapshot ---

TEST(LivePortfolioHelpers, QtProposalSeedingQueuesEveryComponentAtExactUtcScope) {
    ProposalSeedDatabase db;
    const auto nonmidnight = report_date() + std::chrono::hours(17);
    auto result = seed_qt_proposal_positions(
        db, "LIVE_TREND_CARRY_FLAT", {"TREND", "CARRY", "FLAT"},
        "INVESTOR_A", nonmidnight);

    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(db.calls.size(), 3u);
    EXPECT_EQ(db.calls[0].strategy_name, "TREND");
    EXPECT_EQ(db.calls[1].strategy_name, "CARRY");
    EXPECT_EQ(db.calls[2].strategy_name, "FLAT");
    for (const auto& call : db.calls) {
        EXPECT_EQ(call.strategy_id, "LIVE_TREND_CARRY_FLAT");
        EXPECT_EQ(call.portfolio_id, "INVESTOR_A");
        EXPECT_EQ(call.date, "2026-06-02");
    }
    EXPECT_EQ(db.legacy_seed_calls, 0);
}

TEST(LivePortfolioHelpers, QtProposalSeedingStopsAtFirstErrorWithScopeAndCode) {
    ProposalSeedDatabase db;
    db.failing_name = "CARRY";
    auto result = seed_qt_proposal_positions(
        db, "LIVE_TREND_CARRY_FLAT", {"TREND", "CARRY", "FLAT"},
        "INVESTOR_A", report_date());

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);
    EXPECT_EQ(result.error()->component(), "seed_qt_proposal_positions");
    const std::string message = result.error()->what();
    EXPECT_NE(message.find("LIVE_TREND_CARRY_FLAT"), std::string::npos);
    EXPECT_NE(message.find("CARRY"), std::string::npos);
    EXPECT_NE(message.find("INVESTOR_A"), std::string::npos);
    EXPECT_NE(message.find("2026-06-02"), std::string::npos);
    EXPECT_NE(message.find("synthetic proposal seed failure"), std::string::npos);
    ASSERT_EQ(db.calls.size(), 2u);
    EXPECT_EQ(db.calls[0].strategy_name, "TREND");
    EXPECT_EQ(db.calls[1].strategy_name, "CARRY");
    EXPECT_EQ(db.legacy_seed_calls, 0);
}

TEST(LivePortfolioHelpers, QtProposalSeedingRejectsEmptyComponentList) {
    ProposalSeedDatabase db;
    auto result = seed_qt_proposal_positions(
        db, "LIVE_TREND", {}, "INVESTOR_A", report_date());

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(result.error()->component(), "seed_qt_proposal_positions");
    EXPECT_TRUE(db.calls.empty());
    EXPECT_EQ(db.legacy_seed_calls, 0);
}

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

TEST(SetupConsumptionSelection, OrdinaryRecordsReachedDecisionsAndEffectiveWeights) {
    const nlohmann::json config = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.2}}},
        {"B", {{"enabled_live", true}}},
        {"C", {{"enabled_live", false}, {"default_allocation", 0.3}}},
        {"D", {{"default_allocation", 0.4}}}};
    SelectionConsumption read;
    auto result = select_enabled_live_strategies(config, &read);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().names, (std::vector<std::string>{"A", "B"}));
    EXPECT_DOUBLE_EQ(result.value().allocation_sum_before_normalization, 0.7);
    EXPECT_DOUBLE_EQ(result.value().allocations.at("A"), 2.0 / 7.0);
    EXPECT_DOUBLE_EQ(result.value().allocations.at("B"), 5.0 / 7.0);
    auto unobserved = select_enabled_live_strategies(config);
    ASSERT_TRUE(unobserved.is_ok());
    EXPECT_EQ(unobserved.value().names, result.value().names);
    EXPECT_EQ(unobserved.value().allocations, result.value().allocations);
    EXPECT_EQ(unobserved.value().configs, result.value().configs);
    EXPECT_DOUBLE_EQ(unobserved.value().allocation_sum_before_normalization,
                     result.value().allocation_sum_before_normalization);
    ASSERT_EQ(read.ordinary_selection.size(), 4u);
    EXPECT_TRUE(read.controlled_validation.empty());
    EXPECT_DOUBLE_EQ(*read.ordinary_sum, 0.7);
    EXPECT_EQ(read.ordinary_normalized, true);
    EXPECT_EQ(read.ordinary_selection[0].name, "A");
    EXPECT_EQ(read.ordinary_selection[0].enabled_live_present, true);
    EXPECT_EQ(read.ordinary_selection[0].enabled_live_value, true);
    EXPECT_EQ(read.ordinary_selection[0].allocation_defaulted, false);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].allocation_value, 0.2);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].effective_allocation, 2.0 / 7.0);
    EXPECT_EQ(read.ordinary_selection[1].allocation_defaulted, true);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[1].allocation_value, 0.5);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[1].effective_allocation, 5.0 / 7.0);
    EXPECT_EQ(read.ordinary_selection[2].enabled_live_value, false);
    EXPECT_FALSE(read.ordinary_selection[2].allocation_read);
    EXPECT_EQ(read.ordinary_selection[3].enabled_live_present, false);
    EXPECT_FALSE(read.ordinary_selection[3].enabled_live_value.has_value());
    EXPECT_FALSE(read.ordinary_selection[3].allocation_read);
}

TEST(SetupConsumptionSelection, ControlledKeepsValidationAndNestedSelectionReads) {
    const nlohmann::json config = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.4}}},
        {"B", {{"enabled_live", true}, {"default_allocation", 0.6}}},
        {"C", {{"enabled_live", false}, {"default_allocation", 9.0}}},
        {"D", nlohmann::json::object()}};
    SelectionConsumption read;
    auto result = select_controlled_live_strategies(config, &read);
    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(result.value().allocations.at("A"), 0.4);
    EXPECT_DOUBLE_EQ(result.value().allocations.at("B"), 0.6);
    auto unobserved = select_controlled_live_strategies(config);
    ASSERT_TRUE(unobserved.is_ok());
    EXPECT_EQ(unobserved.value().names, result.value().names);
    EXPECT_EQ(unobserved.value().allocations, result.value().allocations);
    EXPECT_EQ(unobserved.value().configs, result.value().configs);
    ASSERT_EQ(read.controlled_validation.size(), 4u);
    ASSERT_EQ(read.ordinary_selection.size(), 4u);
    EXPECT_DOUBLE_EQ(*read.controlled_sum, 1.0);
    EXPECT_EQ(read.controlled_validation[0].enabled_live_defaulted, false);
    EXPECT_DOUBLE_EQ(*read.controlled_validation[0].allocation_value, 0.4);
    EXPECT_DOUBLE_EQ(*read.controlled_validation[0].effective_allocation, 0.4);
    EXPECT_DOUBLE_EQ(*read.controlled_validation[1].effective_allocation, 0.6);
    EXPECT_FALSE(read.controlled_validation[2].allocation_read);
    EXPECT_EQ(read.controlled_validation[3].enabled_live_defaulted, true);
    EXPECT_FALSE(read.controlled_validation[3].allocation_read);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].effective_allocation, 0.4);
}

TEST(SetupConsumptionSelection, FailureKeepsOnlyReachedReadsAndReuseClearsAllStages) {
    SelectionConsumption read;
    ASSERT_TRUE(select_controlled_live_strategies(
        {{"A", {{"enabled_live", true}, {"default_allocation", 1.0}}}}, &read).is_ok());
    const nlohmann::json missing = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.4}}},
        {"B", {{"enabled_live", true}}}};
    auto failed = select_controlled_live_strategies(missing, &read);
    ASSERT_TRUE(failed.is_error());
    EXPECT_EQ(failed.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_STREQ(failed.error()->what(), "runtime_config_invalid");
    ASSERT_EQ(read.controlled_validation.size(), 2u);
    EXPECT_EQ(read.controlled_validation[0].allocation_value, 0.4);
    EXPECT_TRUE(read.controlled_validation[1].allocation_read);
    EXPECT_FALSE(read.controlled_validation[1].allocation_value.has_value());
    EXPECT_TRUE(read.ordinary_selection.empty());
    EXPECT_FALSE(read.ordinary_sum.has_value());
    EXPECT_DOUBLE_EQ(*read.controlled_sum, 0.4);
    auto ordinary = select_enabled_live_strategies(
        {{"ONLY", {{"enabled_live", true}}}}, &read);
    ASSERT_TRUE(ordinary.is_ok());
    EXPECT_TRUE(read.controlled_validation.empty());
    EXPECT_FALSE(read.controlled_sum.has_value());
    ASSERT_EQ(read.ordinary_selection.size(), 1u);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].allocation_value, 0.5);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].effective_allocation, 1.0);
}

TEST(SetupConsumptionSelection, OrdinaryMalformedFlagThrowsAfterReachedRead) {
    SelectionConsumption read;
    const nlohmann::json malformed = {
        {"A", {{"enabled_live", true}, {"default_allocation", 0.25}}},
        {"B", {{"enabled_live", "bad"}}},
        {"C", {{"enabled_live", true}}}};
    EXPECT_THROW(select_enabled_live_strategies(malformed, &read), nlohmann::json::type_error);
    EXPECT_THROW(select_enabled_live_strategies(malformed), nlohmann::json::type_error);
    ASSERT_EQ(read.ordinary_selection.size(), 2u);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].allocation_value, 0.25);
    EXPECT_TRUE(read.ordinary_selection[1].enabled_live_read);
    EXPECT_EQ(read.ordinary_selection[1].enabled_live_present, true);
    EXPECT_FALSE(read.ordinary_selection[1].enabled_live_value.has_value());
    EXPECT_FALSE(read.ordinary_sum.has_value());
}

TEST(SetupConsumptionSelection, ControlledFailuresKeepOriginalCodesAndReachedValues) {
    struct Case {
        nlohmann::json config;
        const char* message;
        size_t reached;
        std::optional<double> last_weight;
        std::optional<double> sum;
    };
    const std::vector<Case> cases = {
        {nlohmann::json::array(), "runtime_config_invalid", 0, std::nullopt, std::nullopt},
        {{{"A", {{"enabled_live", "bad"}}}}, "runtime_config_invalid", 1,
         std::nullopt, std::nullopt},
        {{{"A", {{"enabled_live", true}}}}, "runtime_config_invalid", 1,
         std::nullopt, std::nullopt},
        {{{"A", {{"enabled_live", true}, {"default_allocation", 0.0}}}},
         "runtime_allocation_unsupported", 1, 0.0, std::nullopt},
        {{{"A", {{"enabled_live", true}, {"default_allocation", 1.1}}}},
         "runtime_allocation_unsupported", 1, 1.1, std::nullopt},
        {{{"A", {{"enabled_live", true},
                 {"default_allocation", std::numeric_limits<double>::infinity()}}}},
         "runtime_allocation_unsupported", 1,
         std::numeric_limits<double>::infinity(), std::nullopt},
        {{{"A", {{"enabled_live", true}, {"default_allocation", 0.4}}}},
         "runtime_allocation_unsupported", 1, 0.4, 0.4}};
    SelectionConsumption read;
    for (const auto& entry : cases) {
        auto original = select_controlled_live_strategies(entry.config);
        auto observed = select_controlled_live_strategies(entry.config, &read);
        ASSERT_TRUE(original.is_error());
        ASSERT_TRUE(observed.is_error());
        EXPECT_EQ(observed.error()->code(), original.error()->code());
        EXPECT_STREQ(observed.error()->what(), original.error()->what());
        EXPECT_STREQ(observed.error()->what(), entry.message);
        ASSERT_EQ(read.controlled_validation.size(), entry.reached);
        EXPECT_TRUE(read.ordinary_selection.empty());
        EXPECT_EQ(read.controlled_sum, entry.sum);
        if (entry.reached) {
            EXPECT_EQ(read.controlled_validation.back().allocation_value,
                      entry.last_weight);
        }
    }
}

TEST(SetupConsumptionSelection, OrdinaryNonpositiveSumDoesNotClaimNormalization) {
    SelectionConsumption read;
    auto result = select_enabled_live_strategies(
        {{"A", {{"enabled_live", true}, {"default_allocation", -0.2}}},
         {"B", {{"enabled_live", true}, {"default_allocation", 0.0}}}}, &read);
    ASSERT_TRUE(result.is_ok());
    EXPECT_DOUBLE_EQ(result.value().allocations.at("A"), -0.2);
    EXPECT_DOUBLE_EQ(result.value().allocations.at("B"), 0.0);
    EXPECT_DOUBLE_EQ(*read.ordinary_sum, -0.2);
    EXPECT_EQ(read.ordinary_normalized, false);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[0].effective_allocation, -0.2);
    EXPECT_DOUBLE_EQ(*read.ordinary_selection[1].effective_allocation, 0.0);
    auto early = select_enabled_live_strategies(nlohmann::json::array(), &read);
    ASSERT_TRUE(early.is_error());
    EXPECT_TRUE(read.ordinary_selection.empty());
    EXPECT_FALSE(read.ordinary_sum.has_value());
}

namespace {

template <typename Config>
void expect_all_trend_fields(
    const Config& actual, double weight, double risk_target, double fx_rate, double idm,
    double concentration, bool buffering, double buffer_floor, double buffer_factor,
    const std::vector<std::pair<int, int>>& windows, int short_lookback, int long_lookback,
    size_t history_cap, const std::vector<std::pair<int, double>>& fdm) {
    EXPECT_DOUBLE_EQ(actual.weight, weight);
    EXPECT_DOUBLE_EQ(actual.risk_target, risk_target);
    EXPECT_DOUBLE_EQ(actual.fx_rate, fx_rate);
    EXPECT_DOUBLE_EQ(actual.idm, idm);
    EXPECT_DOUBLE_EQ(actual.max_symbol_concentration, concentration);
    EXPECT_EQ(actual.use_position_buffering, buffering);
    EXPECT_DOUBLE_EQ(actual.carver_buffer_floor, buffer_floor);
    EXPECT_DOUBLE_EQ(actual.carver_buffer_position_factor, buffer_factor);
    EXPECT_EQ(actual.ema_windows, windows);
    EXPECT_EQ(actual.vol_lookback_short, short_lookback);
    EXPECT_EQ(actual.vol_lookback_long, long_lookback);
    EXPECT_EQ(actual.max_history_size, history_cap);
    EXPECT_EQ(actual.fdm, fdm);
}

class OriginalFactoryTrendConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        // These assertions capture the uninitialized stderr sink.
        Logger::reset_for_tests();
        StateManager::reset_instance();
        db_ = std::make_shared<trade_ngin::testing::MockPostgresDatabase>("mock://factory-trend-config");
        ASSERT_TRUE(db_->connect().is_ok());
        base_.max_leverage = 10.0;
        base_.max_drawdown = 0.5;
    }

    void TearDown() override {
        for (auto& strategy : built_) strategy->stop();
        built_.clear();
        db_->disconnect();
        db_.reset();
        StateManager::reset_instance();
    }

    std::shared_ptr<StrategyInterface> build_one(
        const std::string& name, const nlohmann::json& definition,
        std::optional<double> slow_override = std::nullopt) {
        StrategySelection selection;
        selection.names = {name};
        selection.configs.emplace(name, definition);
        selection.allocations.emplace(name, 0.4);
        auto result = build_strategy_instances(selection, base_, 100000.0, defaults_,
                                               slow_override, db_, nullptr);
        EXPECT_EQ(result.size(), 1u);
        built_.insert(built_.end(), result.begin(), result.end());
        return result.at(0);
    }

    StrategyConfig base_;
    StrategyDefaultsConfig defaults_;
    std::shared_ptr<trade_ngin::testing::MockPostgresDatabase> db_;
    std::vector<std::shared_ptr<StrategyInterface>> built_;
};

TEST_F(OriginalFactoryTrendConfigTest, SetupConsumptionRecordsRealProfilesCapitalAndRunningState) {
    StrategySelection selection;
    selection.names = {"STANDARD", "FAST", "SLOW", "MISSING"};
    selection.allocations = {{"STANDARD", 0.1}, {"FAST", 0.2},
                             {"SLOW", 0.3}, {"MISSING", 0.4}};
    selection.configs = {
        {"STANDARD", {{"type", "TrendFollowingStrategy"}}},
        {"FAST", {{"type", "TrendFollowingFastStrategy"}}},
        {"SLOW", {{"type", "TrendFollowingSlowStrategy"}}},
        {"MISSING", nlohmann::json::object()}};
    FactoryConsumption read;
    auto actual = build_strategy_instances(selection, base_, 123457.0, defaults_,
                                           std::nullopt, db_, nullptr, &read);
    built_.insert(built_.end(), actual.begin(), actual.end());
    ASSERT_EQ(actual.size(), 4u);
    EXPECT_NE(std::dynamic_pointer_cast<TrendFollowingStrategy>(actual[0]), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<TrendFollowingFastStrategy>(actual[1]), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<TrendFollowingSlowStrategy>(actual[2]), nullptr);
    EXPECT_NE(std::dynamic_pointer_cast<TrendFollowingStrategy>(actual[3]), nullptr);
    ASSERT_EQ(read.entries.size(), 4u);
    const FactoryProfile profiles[] = {FactoryProfile::Standard, FactoryProfile::Fast,
                                       FactoryProfile::Slow, FactoryProfile::Standard};
    const double allocations[] = {0.1, 0.2, 0.3, 0.4};
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(read.entries[i].name, selection.names[i]);
        EXPECT_EQ(read.entries[i].type_defaulted, i == 3);
        EXPECT_EQ(read.entries[i].profile, profiles[i]);
        EXPECT_DOUBLE_EQ(*read.entries[i].effective_allocation, allocations[i]);
        EXPECT_DOUBLE_EQ(*read.entries[i].initial_capital_argument, 123457.0);
        EXPECT_DOUBLE_EQ(*read.entries[i].allocated_capital, 123457.0 * allocations[i]);
        EXPECT_DOUBLE_EQ(actual[i]->get_config().capital_allocation, 123457.0 * allocations[i]);
        EXPECT_EQ(actual[i]->get_state(), StrategyState::RUNNING);
        EXPECT_EQ(read.entries[i].construction, SetupStage::Succeeded);
        EXPECT_EQ(read.entries[i].initialize, SetupStage::Succeeded);
        EXPECT_EQ(read.entries[i].start, SetupStage::Succeeded);
        EXPECT_FALSE(read.entries[i].initialize_error.has_value());
        EXPECT_FALSE(read.entries[i].start_error.has_value());
    }
    auto unobserved = build_strategy_instances(selection, base_, 123457.0, defaults_,
                                               std::nullopt, db_, nullptr);
    built_.insert(built_.end(), unobserved.begin(), unobserved.end());
    ASSERT_EQ(unobserved.size(), actual.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(unobserved[i]->get_metadata().name, actual[i]->get_metadata().name);
        EXPECT_EQ(typeid(*unobserved[i]), typeid(*actual[i]));
        EXPECT_DOUBLE_EQ(unobserved[i]->get_config().capital_allocation,
                         actual[i]->get_config().capital_allocation);
        EXPECT_EQ(unobserved[i]->get_state(), actual[i]->get_state());
    }
}

TEST_F(OriginalFactoryTrendConfigTest, SetupConsumptionPreservesEarlierStartBeforeLaterUnsupportedType) {
    StrategySelection selection;
    selection.names = {"FIRST", "SECOND"};
    selection.allocations = {{"FIRST", 0.25}, {"SECOND", 0.75}};
    selection.configs = {{"FIRST", {{"type", "TrendFollowingStrategy"}}},
                         {"SECOND", {{"type", "synthetic-private-type"}}}};
    FactoryConsumption read;
    EXPECT_THROW(build_strategy_instances(selection, base_, 90000.0, defaults_,
                                          std::nullopt, db_, nullptr, &read), std::runtime_error);
    ASSERT_EQ(read.entries.size(), 2u);
    EXPECT_EQ(read.entries[0].name, "FIRST");
    EXPECT_EQ(read.entries[0].start, SetupStage::Succeeded);
    EXPECT_EQ(read.entries[1].name, "SECOND");
    EXPECT_EQ(read.entries[1].profile, FactoryProfile::Unsupported);
    EXPECT_EQ(read.entries[1].construction, SetupStage::NotReached);
    EXPECT_EQ(read.entries[1].initialize, SetupStage::NotReached);
    EXPECT_EQ(read.entries[1].start, SetupStage::NotReached);
    EXPECT_EQ(read.entries[1].effective_allocation, 0.75);
    EXPECT_EQ(read.entries[1].allocated_capital, 67500.0);
    // The typed evidence has no field that can retain the raw unsupported type.
}

TEST_F(OriginalFactoryTrendConfigTest, SetupConsumptionRecordsActualInitializeFailureAndResets) {
    StrategySelection selection;
    selection.names = {"BAD"};
    selection.allocations = {{"BAD", 0.6}};
    selection.configs = {{"BAD", {{"type", "TrendFollowingFastStrategy"},
                                   {"config", {{"ema_windows", nlohmann::json::array()}}}}}};
    FactoryConsumption read;
    EXPECT_THROW(build_strategy_instances(selection, base_, 50000.0, defaults_,
                                          std::nullopt, db_, nullptr, &read), std::runtime_error);
    ASSERT_EQ(read.entries.size(), 1u);
    EXPECT_EQ(read.entries[0].profile, FactoryProfile::Fast);
    EXPECT_EQ(read.entries[0].construction, SetupStage::Succeeded);
    EXPECT_EQ(read.entries[0].initialize, SetupStage::Failed);
    EXPECT_TRUE(read.entries[0].initialize_error.has_value());
    EXPECT_EQ(read.entries[0].start, SetupStage::NotReached);
    selection.configs["BAD"] = {{"type", "TrendFollowingFastStrategy"}};
    auto actual = build_strategy_instances(selection, base_, 50000.0, defaults_,
                                           std::nullopt, db_, nullptr, &read);
    built_.insert(built_.end(), actual.begin(), actual.end());
    ASSERT_EQ(read.entries.size(), 1u);
    EXPECT_EQ(read.entries[0].initialize, SetupStage::Succeeded);
    EXPECT_FALSE(read.entries[0].initialize_error.has_value());
    EXPECT_EQ(read.entries[0].start, SetupStage::Succeeded);
}

const std::vector<std::pair<int, double>> kTypedFdm = {
    {1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
const std::vector<std::pair<int, int>> kStandardWindows = {
    {2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
const std::vector<std::pair<int, int>> kFastWindows = {
    {1, 4}, {2, 8}, {4, 16}, {8, 32}, {16, 64}};
const std::vector<std::pair<int, int>> kSlowWindows = {
    {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}, {128, 512}};

struct LiteralTrendFields {
    double weight;
    double risk_target;
    double fx_rate;
    double idm;
    double max_symbol_concentration;
    bool use_position_buffering;
    double carver_buffer_floor;
    double carver_buffer_position_factor;
    std::vector<std::pair<int, int>> ema_windows;
    int vol_lookback_short;
    int vol_lookback_long;
    size_t max_history_size;
    std::vector<std::pair<int, double>> fdm;
};

template <typename Config>
void expect_literal_trend_fields(const Config& actual, const LiteralTrendFields& expected) {
    expect_all_trend_fields(actual, expected.weight, expected.risk_target, expected.fx_rate,
                            expected.idm, expected.max_symbol_concentration,
                            expected.use_position_buffering, expected.carver_buffer_floor,
                            expected.carver_buffer_position_factor, expected.ema_windows,
                            expected.vol_lookback_short, expected.vol_lookback_long,
                            expected.max_history_size, expected.fdm);
}

template <typename Config>
void check_each_factory_leaf_independently(const std::string& type,
                                            const LiteralTrendFields& literal_baseline,
                                            bool opposite_buffering, int normalized_short) {
    StrategyDefaultsConfig defaults;
    // Each invocation starts with a fresh empty JSON config and changes exactly
    // one parsed leaf. All thirteen expected members are checked every time.
    const auto check = [&](const char* key, const nlohmann::json& value,
                           const LiteralTrendFields& expected) {
        const nlohmann::json definition = {{"config", {{key, value}}}};
        const auto result = resolve_factory_trend_config(type, definition, defaults, std::nullopt);
        const auto* actual = std::get_if<Config>(&result);
        ASSERT_NE(actual, nullptr) << key << " / " << type;
        expect_literal_trend_fields(*actual, expected);
    };

    auto want = literal_baseline;
    want.weight = 0.09;
    check("weight", 0.09, want);

    want = literal_baseline;
    want.risk_target = 0.31;
    check("risk_target", 0.31, want);

    want = literal_baseline;
    want.idm = 3.7;
    check("idm", 3.7, want);

    want = literal_baseline;
    want.max_symbol_concentration = 0.27;
    check("max_symbol_concentration", 0.27, want);

    want = literal_baseline;
    want.use_position_buffering = opposite_buffering;
    check("use_position_buffering", opposite_buffering, want);

    want = literal_baseline;
    want.carver_buffer_floor = 0.84;
    check("carver_buffer_floor", 0.84, want);

    want = literal_baseline;
    want.carver_buffer_position_factor = 0.14;
    check("carver_buffer_position_factor", 0.14, want);

    want = literal_baseline;
    want.ema_windows = {{3, 12}, {3, 12}, {5, 20}};
    check("ema_windows", nlohmann::json::array({{3, 12}, {3, 12}, {5, 20}}), want);

    want = literal_baseline;
    want.vol_lookback_short = 37;
    check("vol_lookback_short", 37, want);

    want = literal_baseline;
    want.vol_lookback_long = 370;
    check("vol_lookback_long", 370, want);

    // Zero remains visible at the factory stage; constructor normalization is
    // a separate call and must not be folded into the resolver.
    want = literal_baseline;
    want.vol_lookback_short = 0;
    check("vol_lookback_short", 0, want);
    const auto zero_short = resolve_factory_trend_config(
        type, {{"config", {{"vol_lookback_short", 0}}}}, defaults, std::nullopt);
    auto normalized = std::get<Config>(zero_short);
    normalize_constructor_trend_config(normalized);
    want.vol_lookback_short = normalized_short;
    want.max_history_size = 756;
    expect_literal_trend_fields(normalized, want);
}

}  // namespace

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryPreservesMissingAndEmptyStandardDefaults) {
    auto missing = std::dynamic_pointer_cast<TrendFollowingStrategy>(build_one("STANDARD_MISSING", {{"type", "TrendFollowingStrategy"}}));
    auto empty = std::dynamic_pointer_cast<TrendFollowingStrategy>(build_one("STANDARD_EMPTY", {{"type", "TrendFollowingStrategy"}, {"config", nlohmann::json::object()}}));
    ASSERT_NE(missing, nullptr);
    ASSERT_NE(empty, nullptr);
    expect_all_trend_fields(missing->trend_config_, 1.0, 0.2, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kStandardWindows, 32, 2520, 2520, kTypedFdm);
    expect_all_trend_fields(empty->trend_config_, 0.03, 0.2, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kStandardWindows, 32, 252, 756, kTypedFdm);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryPreservesMissingAndEmptyFastDefaults) {
    auto missing = std::dynamic_pointer_cast<TrendFollowingFastStrategy>(build_one("FAST_MISSING", {{"type", "TrendFollowingFastStrategy"}}));
    auto empty = std::dynamic_pointer_cast<TrendFollowingFastStrategy>(build_one("FAST_EMPTY", {{"type", "TrendFollowingFastStrategy"}, {"config", nlohmann::json::object()}}));
    ASSERT_NE(missing, nullptr);
    ASSERT_NE(empty, nullptr);
    expect_all_trend_fields(missing->trend_config_, 1.0, 0.25, 1.0, 2.5, 0.15, false,
                            0.5, 0.0, kFastWindows, 16, 2520, 2520, kTypedFdm);
    expect_all_trend_fields(empty->trend_config_, 0.03, 0.25, 1.0, 2.5, 0.15, false,
                            0.5, 0.0, kFastWindows, 16, 252, 756, kTypedFdm);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryPreservesSlowOverrideOnlyWhenConfigAbsent) {
    auto missing = std::dynamic_pointer_cast<TrendFollowingSlowStrategy>(build_one("SLOW_MISSING", {{"type", "TrendFollowingSlowStrategy"}}, 0.37));
    auto empty = std::dynamic_pointer_cast<TrendFollowingSlowStrategy>(build_one("SLOW_EMPTY", {{"type", "TrendFollowingSlowStrategy"}, {"config", nlohmann::json::object()}}, 0.37));
    ASSERT_NE(missing, nullptr);
    ASSERT_NE(empty, nullptr);
    expect_all_trend_fields(missing->trend_config_, 0.03, 0.15, 1.0, 2.5, 0.37, true,
                            0.5, 0.0, kSlowWindows, 64, 252, 756, kTypedFdm);
    expect_all_trend_fields(empty->trend_config_, 0.03, 0.15, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kSlowWindows, 64, 252, 756, kTypedFdm);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryDefaultsMissingTypeAtDefinitionParse) {
    auto actual = std::dynamic_pointer_cast<TrendFollowingStrategy>(build_one("MISSING_TYPE", nlohmann::json::object()));
    ASSERT_NE(actual, nullptr);
    expect_all_trend_fields(actual->trend_config_, 1.0, 0.2, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kStandardWindows, 32, 2520, 2520, kTypedFdm);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryReadsOnlyKnownLeavesAndPreservesWindowOrder) {
    defaults_.carver_buffer_floor = 0.73;
    defaults_.carver_buffer_position_factor = 0.11;
    defaults_.fdm = {{9, 9.0}};
    const nlohmann::json definition = {
        {"type", "TrendFollowingFastStrategy"},
        {"config", {{"weight", 0.09}, {"risk_target", 0.31}, {"idm", 3.7},
                    {"max_symbol_concentration", 0.27}, {"use_position_buffering", true},
                    {"carver_buffer_floor", 0.84}, {"carver_buffer_position_factor", 0.14},
                    {"ema_windows", {{3, 12}, {3, 12}, {5, 20}}},
                    {"vol_lookback_short", 37}, {"vol_lookback_long", 370},
                    {"fx_rate", 9.0}, {"max_history_size", 9999}, {"fdm", {{8, 8.0}}},
                    {"private_key", "synthetic-hidden-token"}}}};
    auto actual = std::dynamic_pointer_cast<TrendFollowingFastStrategy>(build_one("FAST_KNOWN", definition));
    ASSERT_NE(actual, nullptr);
    expect_all_trend_fields(actual->trend_config_, 0.09, 0.31, 1.0, 3.7, 0.27, true,
                            0.84, 0.14, {{3, 12}, {3, 12}, {5, 20}}, 37, 370, 756,
                            kTypedFdm);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryUsesChangedCarverDefaultsOnlyWithConfig) {
    defaults_.carver_buffer_floor = 0.73;
    defaults_.carver_buffer_position_factor = 0.11;
    defaults_.fdm = {{9, 9.0}};
    auto missing = std::dynamic_pointer_cast<TrendFollowingStrategy>(build_one("CARVER_MISSING", {{"type", "TrendFollowingStrategy"}}));
    auto empty = std::dynamic_pointer_cast<TrendFollowingStrategy>(build_one("CARVER_EMPTY", {{"type", "TrendFollowingStrategy"}, {"config", nlohmann::json::object()}}));
    ASSERT_NE(missing, nullptr);
    ASSERT_NE(empty, nullptr);
    expect_all_trend_fields(missing->trend_config_, 1.0, 0.2, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kStandardWindows, 32, 2520, 2520, kTypedFdm);
    expect_all_trend_fields(empty->trend_config_, 0.03, 0.2, 1.0, 2.5, 0.15, true,
                            0.73, 0.11, kStandardWindows, 32, 252, 756, kTypedFdm);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryKeepsParseAndLifecycleErrorOrder) {
    StrategySelection selection;
    selection.names = {"BAD"};
    selection.configs["BAD"] = {{"type", 7}};
    ::testing::internal::CaptureStderr();
    EXPECT_THROW(build_strategy_instances(selection, base_, 100000.0, defaults_, std::nullopt,
                                          db_, nullptr), nlohmann::json::type_error);
    auto log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(log.find("Creating strategy: BAD"), std::string::npos);

    selection.configs["BAD"] = {{"type", "TrendFollowingStrategy"}, {"config", 7}};
    ::testing::internal::CaptureStderr();
    EXPECT_THROW(build_strategy_instances(selection, base_, 100000.0, defaults_, std::nullopt,
                                          db_, nullptr), std::out_of_range);
    log = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(log.find("Creating strategy: BAD"), std::string::npos);

    selection.allocations["BAD"] = 0.4;
    ::testing::internal::CaptureStderr();
    EXPECT_THROW(build_strategy_instances(selection, base_, 100000.0, defaults_, std::nullopt,
                                          db_, nullptr), nlohmann::json::type_error);
    log = ::testing::internal::GetCapturedStderr();
    EXPECT_NE(log.find("Creating strategy: BAD"), std::string::npos);

    selection.configs["BAD"] = {{"type", "UnknownSynthetic"}, {"config", 7}};
    ::testing::internal::CaptureStderr();
    EXPECT_THROW(build_strategy_instances(selection, base_, 100000.0, defaults_, std::nullopt,
                                          db_, nullptr), std::runtime_error);
    log = ::testing::internal::GetCapturedStderr();
    const auto created = log.find("Creating strategy: BAD");
    const auto unknown = log.find("Unknown strategy type: UnknownSynthetic for strategy: BAD");
    ASSERT_NE(created, std::string::npos);
    ASSERT_NE(unknown, std::string::npos);
    EXPECT_LT(created, unknown);
    EXPECT_EQ(log.find("synthetic-hidden-token"), std::string::npos);
}

TEST_F(OriginalFactoryTrendConfigTest, OriginalFactoryStartsEarlierEntryBeforeLaterParseFailure) {
    StrategySelection selection;
    selection.names = {"FIRST", "SECOND"};
    selection.allocations = {{"FIRST", 0.4}, {"SECOND", 0.6}};
    selection.configs = {{"FIRST", {{"type", "TrendFollowingStrategy"}}},
                         {"SECOND", {{"type", "TrendFollowingFastStrategy"}, {"config", 7}}}};
    ::testing::internal::CaptureStderr();
    EXPECT_THROW(build_strategy_instances(selection, base_, 100000.0, defaults_, std::nullopt,
                                          db_, nullptr), nlohmann::json::type_error);
    const auto log = ::testing::internal::GetCapturedStderr();
    const auto started = log.find("Strategy FIRST started successfully");
    const auto second = log.find("Creating strategy: SECOND");
    ASSERT_NE(started, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    EXPECT_LT(started, second);
}

TEST(FactoryTrendConfigResolution, ResolvesTypedStandardDefaultsWithoutConstructingStrategy) {
    const nlohmann::json definition = {{"type", "TrendFollowingStrategy"}};
    StrategyDefaultsConfig defaults;
    const auto resolved = resolve_factory_trend_config(
        definition.value("type", "TrendFollowingStrategy"), definition, defaults, std::nullopt);
    const auto* standard = std::get_if<TrendFollowingConfig>(&resolved);
    ASSERT_NE(standard, nullptr);
    expect_all_trend_fields(*standard, 1.0, 0.2, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kStandardWindows, 32, 2520, 0, kTypedFdm);
}

TEST(FactoryTrendConfigResolution, AllTypesKeepMissingDistinctFromEmptyAtFactoryStage) {
    StrategyDefaultsConfig defaults;
    const auto standard_missing = resolve_factory_trend_config(
        "TrendFollowingStrategy", {{"type", "TrendFollowingStrategy"}}, defaults, std::nullopt);
    const auto standard_empty = resolve_factory_trend_config(
        "TrendFollowingStrategy", {{"config", nlohmann::json::object()}}, defaults, std::nullopt);
    const auto fast_missing = resolve_factory_trend_config(
        "TrendFollowingFastStrategy", {{"type", "TrendFollowingFastStrategy"}}, defaults, std::nullopt);
    const auto fast_empty = resolve_factory_trend_config(
        "TrendFollowingFastStrategy", {{"config", nlohmann::json::object()}}, defaults, std::nullopt);
    const auto slow_missing = resolve_factory_trend_config(
        "TrendFollowingSlowStrategy", {{"type", "TrendFollowingSlowStrategy"}}, defaults, 0.37);
    const auto slow_empty = resolve_factory_trend_config(
        "TrendFollowingSlowStrategy", {{"config", nlohmann::json::object()}}, defaults, 0.37);
    ASSERT_TRUE(std::holds_alternative<TrendFollowingConfig>(standard_missing));
    ASSERT_TRUE(std::holds_alternative<TrendFollowingConfig>(standard_empty));
    ASSERT_TRUE(std::holds_alternative<TrendFollowingFastConfig>(fast_missing));
    ASSERT_TRUE(std::holds_alternative<TrendFollowingFastConfig>(fast_empty));
    ASSERT_TRUE(std::holds_alternative<TrendFollowingSlowConfig>(slow_missing));
    ASSERT_TRUE(std::holds_alternative<TrendFollowingSlowConfig>(slow_empty));
    expect_all_trend_fields(std::get<TrendFollowingConfig>(standard_missing),
                            1.0, 0.2, 1.0, 2.5, 0.15, true, 0.5, 0.0,
                            kStandardWindows, 32, 2520, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingConfig>(standard_empty),
                            0.03, 0.2, 1.0, 2.5, 0.15, true, 0.5, 0.0,
                            kStandardWindows, 32, 252, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingFastConfig>(fast_missing),
                            1.0, 0.25, 1.0, 2.5, 0.15, false, 0.5, 0.0,
                            kFastWindows, 16, 2520, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingFastConfig>(fast_empty),
                            0.03, 0.25, 1.0, 2.5, 0.15, false, 0.5, 0.0,
                            kFastWindows, 16, 252, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingSlowConfig>(slow_missing),
                            0.03, 0.15, 1.0, 2.5, 0.37, true, 0.5, 0.0,
                            kSlowWindows, 64, 252, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingSlowConfig>(slow_empty),
                            0.03, 0.15, 1.0, 2.5, 0.15, true, 0.5, 0.0,
                            kSlowWindows, 64, 252, 0, kTypedFdm);
}

TEST(FactoryTrendConfigResolution, EachKnownLeafChangesOnlyItsOwnMemberForAllThreeTypes) {
    // Literal baselines are independent of the resolver and include all thirteen
    // members, including the ordered vectors and the factory-stage zero cap.
    const LiteralTrendFields standard = {
        0.03, 0.2, 1.0, 2.5, 0.15, true, 0.5, 0.0,
        {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}},
        32, 252, 0, {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13},
                     {5, 1.19}, {6, 1.26}}};
    const LiteralTrendFields fast = {
        0.03, 0.25, 1.0, 2.5, 0.15, false, 0.5, 0.0,
        {{1, 4}, {2, 8}, {4, 16}, {8, 32}, {16, 64}},
        16, 252, 0, {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13},
                     {5, 1.19}, {6, 1.26}}};
    const LiteralTrendFields slow = {
        0.03, 0.15, 1.0, 2.5, 0.15, true, 0.5, 0.0,
        {{4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}, {128, 512}},
        64, 252, 0, {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13},
                     {5, 1.19}, {6, 1.26}}};
    check_each_factory_leaf_independently<TrendFollowingConfig>(
        "TrendFollowingStrategy", standard, false, 22);
    check_each_factory_leaf_independently<TrendFollowingFastConfig>(
        "TrendFollowingFastStrategy", fast, true, 16);
    check_each_factory_leaf_independently<TrendFollowingSlowConfig>(
        "TrendFollowingSlowStrategy", slow, false, 64);
}

TEST_F(OriginalFactoryTrendConfigTest, EmptyEmaResolvesThenFailsDuringInitializeForEveryType) {
    struct Case {
        const char* name;
        const char* type;
        size_t variant_index;
    };
    const std::vector<Case> cases = {
        {"EMPTY_EMA_STANDARD", "TrendFollowingStrategy", 1},
        {"EMPTY_EMA_FAST", "TrendFollowingFastStrategy", 2},
        {"EMPTY_EMA_SLOW", "TrendFollowingSlowStrategy", 3}};
    for (const auto& entry : cases) {
        const nlohmann::json definition = {
            {"type", entry.type},
            {"config", {{"ema_windows", nlohmann::json::array()}}}};
        const auto resolved = resolve_factory_trend_config(
            entry.type, definition, defaults_, std::nullopt);
        ASSERT_EQ(resolved.index(), entry.variant_index) << entry.type;
        if (entry.variant_index == 1) {
            EXPECT_TRUE(std::get<TrendFollowingConfig>(resolved).ema_windows.empty());
        } else if (entry.variant_index == 2) {
            EXPECT_TRUE(std::get<TrendFollowingFastConfig>(resolved).ema_windows.empty());
        } else {
            EXPECT_TRUE(std::get<TrendFollowingSlowConfig>(resolved).ema_windows.empty());
        }

        StrategySelection selection;
        selection.names = {entry.name};
        selection.configs[entry.name] = definition;
        selection.allocations[entry.name] = 0.4;
        ::testing::internal::CaptureStderr();
        std::string failure;
        try {
            auto built = build_strategy_instances(selection, base_, 100000.0, defaults_,
                                                  std::nullopt, db_, nullptr);
            for (auto& strategy : built) strategy->stop();
        } catch (const std::runtime_error& error) {
            failure = error.what();
        }
        const auto log = ::testing::internal::GetCapturedStderr();
        ASSERT_FALSE(failure.empty()) << entry.type;
        EXPECT_NE(failure.find("Failed to initialize strategy " + std::string(entry.name)),
                  std::string::npos);
        EXPECT_NE(failure.find("Must specify at least one EMA window pair"),
                  std::string::npos);
        const auto created = log.find("Creating strategy: " + std::string(entry.name));
        const auto initialize_failed =
            log.find("Failed to initialize strategy " + std::string(entry.name));
        ASSERT_NE(created, std::string::npos);
        ASSERT_NE(initialize_failed, std::string::npos);
        EXPECT_LT(created, initialize_failed);
        EXPECT_EQ(log.find("started successfully"), std::string::npos);
    }
}

TEST(FactoryTrendConfigResolution, KnownLeavesAndIgnoredPrivateDataKeepTypedDefaults) {
    StrategyDefaultsConfig defaults;
    defaults.carver_buffer_floor = 0.73;
    defaults.carver_buffer_position_factor = 0.11;
    defaults.fdm = {{9, 9.0}};
    const nlohmann::json configured = {{"config", {
        {"weight", 0.09}, {"risk_target", 0.31}, {"idm", 3.7},
        {"max_symbol_concentration", 0.27}, {"use_position_buffering", false},
        {"carver_buffer_floor", 0.84}, {"carver_buffer_position_factor", 0.14},
        {"ema_windows", {{3, 12}, {3, 12}, {5, 20}}},
        {"vol_lookback_short", 37}, {"vol_lookback_long", 370},
        {"fx_rate", 9.0}, {"max_history_size", 9999}, {"fdm", {{8, 8.0}}},
        {"private_key", "synthetic-hidden-token"}}}};
    ::testing::internal::CaptureStderr();
    const auto standard = resolve_factory_trend_config("TrendFollowingStrategy", configured,
                                                        defaults, std::nullopt);
    const auto fast = resolve_factory_trend_config("TrendFollowingFastStrategy", configured,
                                                    defaults, std::nullopt);
    const auto slow = resolve_factory_trend_config("TrendFollowingSlowStrategy", configured,
                                                    defaults, 0.88);
    const auto output = ::testing::internal::GetCapturedStderr();
    EXPECT_EQ(output.find("synthetic-hidden-token"), std::string::npos);
    expect_all_trend_fields(std::get<TrendFollowingConfig>(standard),
                            0.09, 0.31, 1.0, 3.7, 0.27, false, 0.84, 0.14,
                            {{3, 12}, {3, 12}, {5, 20}}, 37, 370, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingFastConfig>(fast),
                            0.09, 0.31, 1.0, 3.7, 0.27, false, 0.84, 0.14,
                            {{3, 12}, {3, 12}, {5, 20}}, 37, 370, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingSlowConfig>(slow),
                            0.09, 0.31, 1.0, 3.7, 0.27, false, 0.84, 0.14,
                            {{3, 12}, {3, 12}, {5, 20}}, 37, 370, 0, kTypedFdm);
}

TEST(FactoryTrendConfigResolution, ChangedCarverDefaultsApplyOnlyToPresentConfig) {
    StrategyDefaultsConfig defaults;
    defaults.carver_buffer_floor = 0.73;
    defaults.carver_buffer_position_factor = 0.11;
    defaults.fdm = {{9, 9.0}};
    const auto missing = resolve_factory_trend_config("TrendFollowingSlowStrategy",
                                                       nlohmann::json::object(), defaults,
                                                       std::nullopt);
    const auto empty = resolve_factory_trend_config("TrendFollowingSlowStrategy",
        {{"config", nlohmann::json::object()}}, defaults, std::nullopt);
    expect_all_trend_fields(std::get<TrendFollowingSlowConfig>(missing),
                            0.03, 0.15, 1.0, 2.5, 0.15, true, 0.5, 0.0,
                            kSlowWindows, 64, 252, 0, kTypedFdm);
    expect_all_trend_fields(std::get<TrendFollowingSlowConfig>(empty),
                            0.03, 0.15, 1.0, 2.5, 0.15, true, 0.73, 0.11,
                            kSlowWindows, 64, 252, 0, kTypedFdm);
}

TEST(FactoryTrendConfigResolution, UnknownTypeDoesNotReadMalformedConfig) {
    StrategyDefaultsConfig defaults;
    EXPECT_TRUE(std::holds_alternative<std::monostate>(resolve_factory_trend_config(
        "UnknownSynthetic", {{"config", 7}}, defaults, std::nullopt)));
    EXPECT_THROW(resolve_factory_trend_config("TrendFollowingStrategy", {{"config", 7}},
                                              defaults, std::nullopt), nlohmann::json::type_error);
    EXPECT_THROW(resolve_factory_trend_config("TrendFollowingStrategy", {{"config", nullptr}},
                                              defaults, std::nullopt), nlohmann::json::type_error);
    EXPECT_THROW(resolve_factory_trend_config("TrendFollowingFastStrategy",
        {{"config", {{"weight", "wrong"}}}}, defaults, std::nullopt), nlohmann::json::type_error);
}

TEST(FactoryTrendConfigResolution, ResultsOwnTheirDataAcrossInterleavedCalls) {
    StrategyDefaultsConfig defaults;
    nlohmann::json definition = {{"config", {{"ema_windows", {{3, 12}, {3, 12}}}}}};
    const auto first = resolve_factory_trend_config("TrendFollowingStrategy", definition,
                                                    defaults, std::nullopt);
    definition["config"]["ema_windows"] = {{9, 36}};
    const auto second = resolve_factory_trend_config("TrendFollowingFastStrategy", definition,
                                                     defaults, std::nullopt);
    const auto third = resolve_factory_trend_config("TrendFollowingSlowStrategy", definition,
                                                    defaults, std::nullopt);
    EXPECT_EQ(std::get<TrendFollowingConfig>(first).ema_windows,
              (std::vector<std::pair<int, int>>{{3, 12}, {3, 12}}));
    EXPECT_EQ(std::get<TrendFollowingFastConfig>(second).ema_windows,
              (std::vector<std::pair<int, int>>{{9, 36}}));
    EXPECT_EQ(std::get<TrendFollowingSlowConfig>(third).ema_windows,
              (std::vector<std::pair<int, int>>{{9, 36}}));
}

TEST(FactoryTrendConfigResolution, InvalidButParseableValuesStayAtFactoryStage) {
    StrategyDefaultsConfig defaults;
    const auto resolved = resolve_factory_trend_config("TrendFollowingStrategy",
        {{"config", {{"risk_target", -0.2}, {"ema_windows", nlohmann::json::array()},
                      {"vol_lookback_short", -5}, {"vol_lookback_long", -5}}}},
        defaults, std::nullopt);
    const auto& config = std::get<TrendFollowingConfig>(resolved);
    EXPECT_DOUBLE_EQ(config.risk_target, -0.2);
    EXPECT_TRUE(config.ema_windows.empty());
    EXPECT_EQ(config.vol_lookback_short, -5);
    EXPECT_EQ(config.vol_lookback_long, -5);
    EXPECT_EQ(config.max_history_size, 0u);
    auto normalized = config;
    normalize_constructor_trend_config(normalized);
    EXPECT_EQ(normalized.vol_lookback_short, 22);
    EXPECT_EQ(normalized.vol_lookback_long, 88);
    EXPECT_EQ(normalized.max_history_size, 756u);
    EXPECT_DOUBLE_EQ(normalized.risk_target, -0.2);
    EXPECT_TRUE(normalized.ema_windows.empty());
}

TEST(FactoryTrendConfigResolution, PureStagesComposeIdempotentlyForDefinedInputs) {
    StrategyDefaultsConfig defaults;
    auto standard = std::get<TrendFollowingConfig>(resolve_factory_trend_config(
        "TrendFollowingStrategy", {{"config", {{"vol_lookback_short", 900},
                                        {"vol_lookback_long", 900}}}}, defaults, std::nullopt));
    normalize_constructor_trend_config(standard);
    EXPECT_EQ(standard.vol_lookback_short, 900);
    EXPECT_EQ(standard.vol_lookback_long, 3600);
    EXPECT_EQ(standard.max_history_size, 3600u);
    const auto once = standard;
    normalize_constructor_trend_config(standard);
    expect_all_trend_fields(standard, 0.03, 0.2, 1.0, 2.5, 0.15, true,
                            0.5, 0.0, kStandardWindows, 900, 3600, 3600, kTypedFdm);
    EXPECT_EQ(standard.vol_lookback_long, once.vol_lookback_long);

    auto fast = std::get<TrendFollowingFastConfig>(resolve_factory_trend_config(
        "TrendFollowingFastStrategy", {{"config", nlohmann::json::object()}},
        defaults, std::nullopt));
    normalize_constructor_trend_config(fast);
    expect_all_trend_fields(fast, 0.03, 0.25, 1.0, 2.5, 0.15, false,
                            0.5, 0.0, kFastWindows, 16, 252, 756, kTypedFdm);
    normalize_constructor_trend_config(fast);
    EXPECT_EQ(fast.max_history_size, 756u);

    auto slow = std::get<TrendFollowingSlowConfig>(resolve_factory_trend_config(
        "TrendFollowingSlowStrategy", nlohmann::json::object(), defaults, 0.37));
    normalize_constructor_trend_config(slow);
    expect_all_trend_fields(slow, 0.03, 0.15, 1.0, 2.5, 0.37, true,
                            0.5, 0.0, kSlowWindows, 64, 252, 756, kTypedFdm);
    normalize_constructor_trend_config(slow);
    EXPECT_EQ(slow.max_history_size, 756u);
}

namespace {
StrategySelection inspection_selection(const AppConfig& app,
    const std::vector<std::pair<std::string, double>>& chosen) {
    StrategySelection selection;
    for (const auto& [id, allocation] : chosen) {
        selection.names.push_back(id);
        selection.allocations.emplace(id, allocation);
        selection.configs.emplace(id, app.strategies_config.at(id));
    }
    return selection;
}

void expect_unavailable_capture(const nlohmann::json& capture, const char* reason) {
    EXPECT_EQ(capture.size(), 4u);
    EXPECT_EQ(capture.at("status"), "unavailable");
    EXPECT_EQ(capture.at("reason"), reason);
    EXPECT_TRUE(capture.at("supplied").is_null());
    EXPECT_TRUE(capture.at("selected_trend").is_null());
}
}

TEST(LiveConfigInspectionCapture, PreservesSelectedOrderMissingTypeAndAllTypedStageMembers) {
    AppConfig app;
    app.database.password = "synthetic-db-secret";
    app.email.password = "synthetic-email-secret";
    app.strategies_config = {
        {"SLOW", {{"enabled_live", true}, {"default_allocation", 0.4},
                  {"type", "TrendFollowingSlowStrategy"}, {"private", "synthetic-hidden"}}},
        {"STANDARD", {{"enabled_live", true}, {"default_allocation", 0.6}}},
        {"DISABLED", {{"enabled_live", false}, {"type", "UnknownSynthetic"}}}};
    const auto selection = inspection_selection(app, {{"SLOW", 0.4}, {"STANDARD", 0.6}});
    const auto capture = build_live_config_inspection_capture(app, selection, 0.37);
    ASSERT_EQ(capture.size(), 4u);
    EXPECT_EQ(capture.at("status"), "available");
    EXPECT_EQ(capture.at("reason"), "none");
    const auto& selected = capture.at("selected_trend");
    ASSERT_EQ(selected.size(), 4u);
    EXPECT_EQ(selected.at("schema_version"), 1);
    EXPECT_EQ(selected.at("provenance"), "shared_resolver_same_inputs");
    EXPECT_EQ(selected.at("slow_concentration_override"),
              (nlohmann::json{{"state", "present"}, {"value", 0.37}}));
    ASSERT_EQ(selected.at("strategies").size(), 2u);
    const auto& slow = selected.at("strategies").at(0);
    EXPECT_EQ(slow.size(), 5u);
    EXPECT_EQ(slow.at("strategy_id"), "SLOW");
    EXPECT_EQ(slow.at("strategy_type"), "TrendFollowingSlowStrategy");
    EXPECT_EQ(slow.at("selected_allocation"), 0.4);
    const nlohmann::json slow_factory = {
        {"weight", 0.03}, {"risk_target", 0.15}, {"fx_rate", 1.0}, {"idm", 2.5},
        {"max_symbol_concentration", 0.37}, {"use_position_buffering", true},
        {"carver_buffer_floor", 0.5}, {"carver_buffer_position_factor", 0.0},
        {"ema_windows", {{4,16},{8,32},{16,64},{32,128},{64,256},{128,512}}},
        {"vol_lookback_short", 64}, {"vol_lookback_long", 252},
        {"max_history_size", 0u},
        {"fdm", {{1,1.0},{2,1.03},{3,1.08},{4,1.13},{5,1.19},{6,1.26}}}};
    EXPECT_EQ(slow.at("factory_resolved"), slow_factory);
    auto slow_normalized = slow_factory;
    slow_normalized["max_history_size"] = 756u;
    EXPECT_EQ(slow.at("constructor_normalized"), slow_normalized);
    const auto& standard = selected.at("strategies").at(1);
    EXPECT_EQ(standard.at("strategy_id"), "STANDARD");
    EXPECT_EQ(standard.at("strategy_type"), "TrendFollowingStrategy");
    EXPECT_EQ(standard.at("selected_allocation"), 0.6);
    const nlohmann::json standard_factory = {
        {"weight", 1.0}, {"risk_target", 0.2}, {"fx_rate", 1.0}, {"idm", 2.5},
        {"max_symbol_concentration", 0.15}, {"use_position_buffering", true},
        {"carver_buffer_floor", 0.5}, {"carver_buffer_position_factor", 0.0},
        {"ema_windows", {{2,8},{4,16},{8,32},{16,64},{32,128},{64,256}}},
        {"vol_lookback_short", 32}, {"vol_lookback_long", 2520},
        {"max_history_size", 0u},
        {"fdm", {{1,1.0},{2,1.03},{3,1.08},{4,1.13},{5,1.19},{6,1.26}}}};
    EXPECT_EQ(standard.at("factory_resolved"), standard_factory);
    auto standard_normalized = standard_factory;
    standard_normalized["max_history_size"] = 2520u;
    EXPECT_EQ(standard.at("constructor_normalized"), standard_normalized);
    const auto supplied = capture.at("supplied");
    const auto& fields = supplied.at("fields");
    auto type_row = std::find_if(fields.begin(), fields.end(), [](const auto& row) {
        return row.at("path") == "/strategies/STANDARD/type";
    });
    ASSERT_NE(type_row, fields.end());
    EXPECT_EQ(type_row->at("value_state"), "absent_in_input");
    EXPECT_FALSE(type_row->contains("value"));
    EXPECT_EQ(capture.dump().find("synthetic-"), std::string::npos);
}

TEST(LiveConfigInspectionCapture, EmptyConfigDiffersFromMissingAndIgnoredLeavesDoNotEscape) {
    AppConfig app;
    app.strategies_config = {
        {"FAST", {{"enabled_live", true}, {"default_allocation", 1.0},
                   {"type", "TrendFollowingFastStrategy"},
                   {"config", {{"fx_rate", 42.0}, {"max_history_size", 9999},
                               {"fdm", {{9, 9.0}}}, {"secret", "synthetic-hidden"}}}}}};
    const auto capture = build_live_config_inspection_capture(
        app, inspection_selection(app, {{"FAST", 1.0}}), std::nullopt);
    ASSERT_EQ(capture.at("status"), "available");
    EXPECT_EQ(capture.at("selected_trend").at("slow_concentration_override"),
              (nlohmann::json{{"state", "absent"}}));
    const auto& row = capture.at("selected_trend").at("strategies").at(0);
    const nlohmann::json factory = {
        {"weight", 0.03}, {"risk_target", 0.25}, {"fx_rate", 1.0}, {"idm", 2.5},
        {"max_symbol_concentration", 0.15}, {"use_position_buffering", false},
        {"carver_buffer_floor", 0.5}, {"carver_buffer_position_factor", 0.0},
        {"ema_windows", {{1,4},{2,8},{4,16},{8,32},{16,64}}},
        {"vol_lookback_short", 16}, {"vol_lookback_long", 252},
        {"max_history_size", 0u},
        {"fdm", {{1,1.0},{2,1.03},{3,1.08},{4,1.13},{5,1.19},{6,1.26}}}};
    EXPECT_EQ(row.at("factory_resolved"), factory);
    auto normalized = factory;
    normalized["max_history_size"] = 756u;
    EXPECT_EQ(row.at("constructor_normalized"), normalized);
    EXPECT_EQ(capture.dump().find("synthetic-hidden"), std::string::npos);
}

TEST(LiveConfigInspectionCapture, InvalidProjectionAndUnsafeSelectedShapesAreRedacted) {
    AppConfig app;
    app.strategies_config = {{"GOOD", {{"enabled_live", true},
        {"default_allocation", 1.0}, {"type", "TrendFollowingStrategy"}}}};
    const auto good = inspection_selection(app, {{"GOOD", 1.0}});
    app.strategies_config["DISABLED"] = {{"enabled_live", false},
        {"config", {{"ema_windows", {{3}}}, {"private", "synthetic-secret"}}}};
    expect_unavailable_capture(build_live_config_inspection_capture(app, good, std::nullopt),
                               "projection_invalid");
    app.strategies_config.erase("DISABLED");
    auto mismatch = good;
    mismatch.allocations["EXTRA"] = 0.1;
    expect_unavailable_capture(build_live_config_inspection_capture(app, mismatch, std::nullopt),
                               "selected_stage_unavailable");
    mismatch = good;
    mismatch.allocations["GOOD"] = std::numeric_limits<double>::infinity();
    expect_unavailable_capture(build_live_config_inspection_capture(app, mismatch, std::nullopt),
                               "selected_stage_unavailable");
    app.strategies_config["GOOD"]["config"] = {{"vol_lookback_short", 536870911},
                                                  {"vol_lookback_long", 1}};
    auto safe_boundary = build_live_config_inspection_capture(
        app, inspection_selection(app, {{"GOOD", 1.0}}), std::nullopt);
    ASSERT_EQ(safe_boundary.at("status"), "available");
    const auto& safe_row = safe_boundary.at("selected_trend").at("strategies").at(0);
    EXPECT_EQ(safe_row.at("factory_resolved").at("vol_lookback_short"), 536870911);
    EXPECT_EQ(safe_row.at("constructor_normalized").at("vol_lookback_long"), 2147483644);
    app.strategies_config["GOOD"]["config"]["vol_lookback_short"] = 536870912;
    expect_unavailable_capture(build_live_config_inspection_capture(
        app, inspection_selection(app, {{"GOOD", 1.0}}), std::nullopt),
                               "selected_stage_unavailable");
    app.strategies_config["GOOD"]["config"] = {{"ema_windows", {{1, 2, 3}}}};
    expect_unavailable_capture(build_live_config_inspection_capture(app, good, std::nullopt),
                               "projection_invalid");
}

TEST(LiveConfigInspectionCapture, SelectedUnknownTypeAndOversizeProjectionRemainUnavailable) {
    AppConfig app;
    app.strategies_config = {{"GOOD", {{"enabled_live", true},
        {"default_allocation", 1.0}, {"type", "UnknownSynthetic"},
        {"config", {{"private", "synthetic-hidden"}}}}}};
    auto selected = inspection_selection(app, {{"GOOD", 1.0}});
    expect_unavailable_capture(build_live_config_inspection_capture(app, selected, std::nullopt),
                               "selected_stage_unavailable");
    app.strategies_config["GOOD"]["type"] = "TrendFollowingStrategy";
    selected = inspection_selection(app, {{"GOOD", 1.0}});
    for (int index = 0; index < 700; ++index) {
        const std::string id = "D" + std::string(110, 'A') + std::to_string(index);
        app.strategies_config[id] = {{"enabled_live", false},
                                     {"type", "TrendFollowingStrategy"}};
    }
    expect_unavailable_capture(build_live_config_inspection_capture(app, selected, std::nullopt),
                               "capture_failed");
}

TEST(LiveConfigInspectionCapture, ThreeTypesPreserveMissingVersusEmptyAcrossBothStages) {
    struct Case {
        const char* type;
        bool empty_config;
        double weight;
        double risk_target;
        int short_window;
        int long_window;
        std::uint64_t normalized_capacity;
        bool buffering;
        double concentration;
    };
    const std::vector<Case> cases = {
        {"TrendFollowingStrategy", false, 1.0, 0.2, 32, 2520, 2520, true, 0.15},
        {"TrendFollowingStrategy", true, 0.03, 0.2, 32, 252, 756, true, 0.15},
        {"TrendFollowingFastStrategy", false, 1.0, 0.25, 16, 2520, 2520, false, 0.15},
        {"TrendFollowingFastStrategy", true, 0.03, 0.25, 16, 252, 756, false, 0.15},
        {"TrendFollowingSlowStrategy", false, 0.03, 0.15, 64, 252, 756, true, 0.37},
        {"TrendFollowingSlowStrategy", true, 0.03, 0.15, 64, 252, 756, true, 0.15},
    };
    for (const auto& expected : cases) {
        AppConfig app;
        nlohmann::json definition = {{"enabled_live", true}, {"default_allocation", 1.0},
                                      {"type", expected.type}};
        if (expected.empty_config) definition["config"] = nlohmann::json::object();
        app.strategies_config = {{"TREND", definition}};
        const auto capture = build_live_config_inspection_capture(
            app, inspection_selection(app, {{"TREND", 1.0}}), 0.37);
        ASSERT_EQ(capture.at("status"), "available") << expected.type;
        const auto& row = capture.at("selected_trend").at("strategies").at(0);
        const auto& factory = row.at("factory_resolved");
        const auto& normalized = row.at("constructor_normalized");
        ASSERT_EQ(factory.size(), 13u);
        ASSERT_EQ(normalized.size(), 13u);
        EXPECT_EQ(factory.at("weight"), expected.weight);
        EXPECT_EQ(factory.at("risk_target"), expected.risk_target);
        EXPECT_EQ(factory.at("vol_lookback_short"), expected.short_window);
        EXPECT_EQ(factory.at("vol_lookback_long"), expected.long_window);
        EXPECT_EQ(factory.at("max_history_size"), 0u);
        EXPECT_EQ(factory.at("use_position_buffering"), expected.buffering);
        EXPECT_EQ(factory.at("max_symbol_concentration"), expected.concentration);
        EXPECT_EQ(normalized.at("weight"), expected.weight);
        EXPECT_EQ(normalized.at("vol_lookback_short"), expected.short_window);
        EXPECT_EQ(normalized.at("vol_lookback_long"), expected.long_window);
        EXPECT_EQ(normalized.at("max_history_size"), expected.normalized_capacity);
    }
}

TEST(RecordedConfiguration, Schema2PreservesRiskModulesAndRawWeights) {
    AppConfig config;
    config.portfolio_id="REPLAY";
    config.use_optimization=true;
    config.covariance_history_prices=123;
    config.max_drawdown=config.risk_schema.max_drawdown=.3;
    config.max_leverage=config.risk_schema.max_leverage=2;
    config.risk_schema.reporting={"carver","all_bars",.25,.05,.85,4,2,.99,252};
    config.risk_schema.attribution={{"_ruled_by","test"},{"_ruled_on","2026-10-01"}};
    config.strategies_config={{"TREND",{{"type","TrendFollowingStrategy"},{"enabled_live",true},{"default_allocation",2.0}}}};
    config.risk_schema.portfolio.push_back(make_none_module("recorded fixture","test","2026-10-01").value());
    config.risk_schema.sleeves["TREND"].push_back({"sleeve_scale","constant_scale",ConstantScaleModuleConfig{.8,false}});
    auto snapshot=build_runtime_trading_snapshot(config);ASSERT_TRUE(snapshot.is_ok());
    auto parsed=ConfigLoader::parse_trading_config(snapshot.value());
    ASSERT_TRUE(parsed.is_ok())<<(parsed.is_error()?parsed.error()->what():"");
    auto rebuilt=build_runtime_trading_snapshot(parsed.value());ASSERT_TRUE(rebuilt.is_ok());
    ASSERT_EQ(rebuilt.value(),snapshot.value());
    auto result=replay_portfolio_config(snapshot.value());ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().use_optimization);
    EXPECT_EQ(result.value().covariance_history_prices,123);
    EXPECT_EQ(result.value().risk_modules.size(),1);
    EXPECT_EQ(result.value().sleeve_risk_modules.size(),1);
    EXPECT_TRUE(result.value().use_risk_management);
    auto bad=snapshot.value();bad["snapshot_version"]=3;EXPECT_TRUE(replay_portfolio_config(bad).is_error());
    bad=snapshot.value();bad["risk"]["fabricated"]=true;EXPECT_TRUE(replay_portfolio_config(bad).is_error());
}
TEST(RecordedConfiguration, HistoricalFlatRiskRemainsReadable) {
    nlohmann::json snapshot={{"snapshot_version",1},{"initial_capital",12345.0},
        {"risk",{{"var_limit",.17}}},{"strategy_defaults",{{"use_optimization",false},{"use_risk_management",true}}}};
    auto result=replay_portfolio_config(snapshot);ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().use_optimization);
    EXPECT_TRUE(result.value().use_risk_management);
    EXPECT_DOUBLE_EQ(result.value().risk_config.var_limit,.17);
    EXPECT_TRUE(result.value().risk_modules.empty());
}
