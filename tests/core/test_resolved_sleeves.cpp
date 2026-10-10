// tests/core/test_resolved_sleeves.cpp
//
// T-8D R24 and R25, T-8D-2 R53 and R73, LOOP_SPEC sections 7.5 and 7.5.1 -- the JSON a run stores
// about its own configuration (core/resolved_sleeves.hpp): each sleeve's resolved values in
// trading.live_results.config, the two keys on the day's sizing capital, the strategy_type key,
// and the keys backtest.run_metadata.portfolio_config gains.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/resolved_sleeves.hpp"
#include "trade_ngin/strategy/sleeve_config.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;

namespace {

nlohmann::json read_template(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            return nlohmann::json::parse(in);
        }
        dir = dir.parent_path();
    }
    return nullptr;
}

// A book's sleeves resolved the way the futures runners resolve them: the FAST type starts from
// fast_trend_following_config(), the required keys of the sleeve's "config" object are read over
// it, and the allocations are default_allocation (0.5 when absent) normalised to sum to 1.
std::vector<ResolvedSleeve> resolve_book(const nlohmann::json& strategies) {
    std::vector<ResolvedSleeve> sleeves;
    double total = 0.0;
    for (const auto& [id, def] : strategies.items()) total += def.value("default_allocation", 0.5);
    for (const auto& [id, def] : strategies.items()) {
        TrendFollowingConfig config = def.value("type", "") == "TrendFollowingFastStrategy"
                                          ? fast_trend_following_config()
                                          : TrendFollowingConfig{};
        EXPECT_TRUE(read_required_sleeve_keys(id, def, config).is_ok()) << id;
        sleeves.push_back(
            {id, config.idm, config.risk_target, def.value("default_allocation", 0.5) / total});
    }
    return sleeves;
}

std::set<std::string> keys_of(const nlohmann::json& j) {
    std::set<std::string> keys;
    for (const auto& item : j.items()) keys.insert(item.key());
    return keys;
}

}  // namespace

// R25's defect: BASE's FAST sleeve resolves risk_target 0.25 and the string stored 0.2.
TEST(ResolvedSleeves, ABaseConfigWhoseFastSleeveResolvesAQuarterStoresAQuarter) {
    const auto portfolio = read_template("config_template/portfolios/base/portfolio.json");
    if (portfolio.is_null()) GTEST_SKIP() << "config template not found from the test directory";
    const auto sleeves = resolve_book(portfolio.at("strategies"));
    const std::string stored =
        futures_live_results_config_json("LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST", sleeves,
                                         500000.0, 30, 600000.0, 100000.0)
            .dump();

    const auto parsed = nlohmann::json::parse(stored);
    ASSERT_TRUE(parsed.contains("sleeves"));
    const auto& fast = parsed.at("sleeves").at("TREND_FOLLOWING_FAST");
    EXPECT_DOUBLE_EQ(fast.at("risk_target").get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(fast.at("idm").get<double>(), 2.5);
    EXPECT_DOUBLE_EQ(fast.at("allocation").get<double>(), 0.3);
    const auto& trend = parsed.at("sleeves").at("TREND_FOLLOWING");
    EXPECT_DOUBLE_EQ(trend.at("risk_target").get<double>(), 0.2);
    EXPECT_DOUBLE_EQ(trend.at("idm").get<double>(), 2.5);
    EXPECT_DOUBLE_EQ(trend.at("allocation").get<double>(), 0.7);
    EXPECT_EQ(keys_of(fast), (std::set<std::string>{"allocation", "idm", "risk_target"}));
    // The removed TrendFollowingConfig::weight has no key: the allocation is stored as what it is.
    EXPECT_EQ(stored.find("config_weight"), std::string::npos);
}

TEST(ResolvedSleeves, AOneSleeveBookStoresItsOwnValues) {
    const auto portfolio = read_template("config_template/portfolios/conservative/portfolio.json");
    if (portfolio.is_null()) GTEST_SKIP() << "config template not found from the test directory";
    nlohmann::json live = nlohmann::json::object();
    for (const auto& [id, def] : portfolio.at("strategies").items()) {
        if (def.value("enabled_live", false)) live[id] = def;
    }
    ASSERT_EQ(live.size(), 1u);
    const auto sleeves = resolve_book(live);
    const auto j = resolved_sleeves_json(sleeves);
    const auto& only = j.begin().value();
    const auto& config = live.begin().value().at("config");
    EXPECT_DOUBLE_EQ(only.at("risk_target").get<double>(), config.at("risk_target").get<double>());
    EXPECT_DOUBLE_EQ(only.at("idm").get<double>(), config.at("idm").get<double>());
    EXPECT_DOUBLE_EQ(only.at("allocation").get<double>(), 1.0);
}

// R73: AlgoLens keys on config["strategy_type"]: top level, a JSON string, the row's strategy_id
// byte for byte, read back from the stored string.
TEST(ResolvedSleeves, StrategyTypeParsesBackAsTheRowsStrategyId) {
    for (const std::string strategy_id :
         {"LIVE_TREND_FOLLOWING", "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST"}) {
        const std::string stored =
            futures_live_results_config_json(strategy_id, {{"TREND_FOLLOWING", 2.5, 0.2, 1.0}},
                                             500000.0, 1, 1.0, 1.0)
                .dump();
        const auto parsed = nlohmann::json::parse(stored);
        ASSERT_TRUE(parsed.is_object());
        ASSERT_TRUE(parsed.contains("strategy_type"));
        ASSERT_TRUE(parsed.at("strategy_type").is_string());
        EXPECT_EQ(parsed.at("strategy_type").get<std::string>(), strategy_id);
        EXPECT_NE(stored.find("\"strategy_type\":\"" + strategy_id + "\""), std::string::npos);
        EXPECT_FALSE(parsed.at("sleeves").contains("strategy_type"));
    }
}

// R24: capital_allocation is the day's sizing capital and gross_leverage the gross notional over it.
TEST(ResolvedSleeves, TheCapitalKeysAreOnTheDaysSizingCapital) {
    const auto j = futures_live_results_config_json("LIVE_TREND_FOLLOWING",
                                                    {{"TREND_FOLLOWING", 2.5, 0.2, 1.0}}, 480000.0,
                                                    12, 600000.0, -120000.0);
    EXPECT_DOUBLE_EQ(j.at("capital_allocation").get<double>(), 480000.0);
    EXPECT_DOUBLE_EQ(j.at("gross_leverage").get<double>(), 600000.0 / 480000.0);
    EXPECT_EQ(j.at("active_positions").get<int>(), 12);
    EXPECT_DOUBLE_EQ(j.at("gross_notional").get<double>(), 600000.0);
    EXPECT_DOUBLE_EQ(j.at("net_notional").get<double>(), -120000.0);
    EXPECT_EQ(keys_of(j), (std::set<std::string>{"active_positions", "capital_allocation",
                                                 "gross_leverage", "gross_notional", "idm",
                                                 "net_notional", "risk_target", "sleeves",
                                                 "strategy_type", "weight"}));
}

// A row with no sizing capital to state (a sizing hold) carries neither key: no number is made up.
TEST(ResolvedSleeves, ARowWithNoSizingCapitalCarriesNeitherCapitalKey) {
    for (const std::optional<double> none : {std::optional<double>{}, std::optional<double>{0.0}}) {
        const auto j = futures_live_results_config_json(
            "LIVE_TREND_FOLLOWING", {{"TREND_FOLLOWING", 2.5, 0.2, 1.0}}, none, 12, 600000.0,
            -120000.0);
        EXPECT_FALSE(j.contains("capital_allocation"));
        EXPECT_FALSE(j.contains("gross_leverage"));
        EXPECT_TRUE(j.contains("sleeves"));
        EXPECT_EQ(j.at("strategy_type").get<std::string>(), "LIVE_TREND_FOLLOWING");
    }
}

TEST(ResolvedSleeves, TheSizingCapitalIsRiskDetailsThenTheRunsReadThenNone) {
    // The row stores risk_detail: its sizing_capital.
    EXPECT_EQ(stored_config_sizing_capital(true, 481000.0, true, 482000.0), 481000.0);
    // No risk_detail (the overlay refused, nothing sized): the capital the run read and set.
    EXPECT_EQ(stored_config_sizing_capital(false, 0.0, true, 482000.0), 482000.0);
    // A sizing hold read none.
    EXPECT_EQ(stored_config_sizing_capital(false, 0.0, false, 0.0), std::nullopt);
}

// The top-level weight, risk_target and idm are the literals the string has always carried.
TEST(ResolvedSleeves, TheTopLevelLiteralsAreKeptForOutsideReaders) {
    const auto j = futures_live_results_config_json(
        "LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST",
        {{"TREND_FOLLOWING", 2.4, 0.21, 0.7}, {"TREND_FOLLOWING_FAST", 2.6, 0.25, 0.3}}, 500000.0,
        1, 1.0, 1.0);
    EXPECT_DOUBLE_EQ(j.at("weight").get<double>(), 0.03);
    EXPECT_DOUBLE_EQ(j.at("risk_target").get<double>(), 0.2);
    EXPECT_DOUBLE_EQ(j.at("idm").get<double>(), 2.5);
    EXPECT_DOUBLE_EQ(j.at("sleeves").at("TREND_FOLLOWING").at("idm").get<double>(), 2.4);
    EXPECT_DOUBLE_EQ(j.at("sleeves").at("TREND_FOLLOWING").at("risk_target").get<double>(), 0.21);
}

// A sleeve stores only the keys it resolves: the equity mean reversion sleeve has no idm.
TEST(ResolvedSleeves, ASleeveWithNoIdmStoresNoIdmKey) {
    const auto j = resolved_sleeves_json({{"EQUITY_MEAN_REVERSION", std::nullopt, 0.15, 1.0}});
    EXPECT_EQ(keys_of(j.at("EQUITY_MEAN_REVERSION")),
              (std::set<std::string>{"allocation", "risk_target"}));
    EXPECT_DOUBLE_EQ(j.at("EQUITY_MEAN_REVERSION").at("risk_target").get<double>(), 0.15);
}

// R53 and LOOP_SPEC 7.5.1: the keys backtest.run_metadata.portfolio_config gains, and only those.
TEST(ResolvedSleeves, TheBacktestRunKeysArePinned) {
    nlohmann::json portfolio_config = {{"total_capital", 500000.0},
                                       {"strategy_allocations", {{"TREND_FOLLOWING", 1.0}}}};
    portfolio_config[kSleevesKey] =
        resolved_sleeves_json({{"TREND_FOLLOWING", 2.5, 0.2, 1.0}}, false);
    // A futures backtest: its calculator applies 252, its grid's ruled factor is 311.0574.
    add_backtest_run_keys(portfolio_config, 2, "2026-05-03", 256, 252.0, 311.0574);

    EXPECT_EQ(keys_of(portfolio_config),
              (std::set<std::string>{"frozen_end_date", "lookback_years", "sleeves",
                                     "statistics_K", "statistics_K_ruled_not_applied",
                                     "strategy_allocations", "total_capital", "warmup_days"}));
    EXPECT_EQ(portfolio_config.at("lookback_years").get<int>(), 2);
    EXPECT_EQ(portfolio_config.at("frozen_end_date").get<std::string>(), "2026-05-03");
    EXPECT_EQ(portfolio_config.at("warmup_days").get<int>(), 256);
    // The recorded factor is the one the stored figures use; the ruled one is named as not applied.
    EXPECT_DOUBLE_EQ(portfolio_config.at("statistics_K").get<double>(), 252.0);
    EXPECT_DOUBLE_EQ(portfolio_config.at("statistics_K_ruled_not_applied").get<double>(), 311.0574);
    EXPECT_STREQ(kStatisticsKRuledNotAppliedKey, "statistics_K_ruled_not_applied");
    EXPECT_DOUBLE_EQ(portfolio_config.at("total_capital").get<double>(), 500000.0);
    EXPECT_FALSE(portfolio_config.contains("sizing_capital"));
    // The allocation is strategy_allocations' and is not written a second time.
    EXPECT_EQ(keys_of(portfolio_config.at("sleeves").at("TREND_FOLLOWING")),
              (std::set<std::string>{"idm", "risk_target"}));
    EXPECT_STREQ(kStatisticsKKey, "statistics_K");
    EXPECT_STREQ(kSleevesKey, "sleeves");
}

TEST(ResolvedSleeves, AnUnsetFrozenEndDateIsStoredAsNull) {
    nlohmann::json portfolio_config = nlohmann::json::object();
    // An equity backtest: the ruled factor is the one applied, so nothing is left to name.
    add_backtest_run_keys(portfolio_config, 5, "", 0, 252.0, 252.0);
    EXPECT_TRUE(portfolio_config.at("frozen_end_date").is_null());
    EXPECT_DOUBLE_EQ(portfolio_config.at("statistics_K").get<double>(), 252.0);
    EXPECT_FALSE(portfolio_config.contains("statistics_K_ruled_not_applied"));
}

TEST(ResolvedSleeves, TheLogLineNamesEverySleevesValues) {
    EXPECT_EQ(resolved_sleeves_log_line("BASE_PORTFOLIO",
                                        {{"TREND_FOLLOWING", 2.5, 0.2, 0.7},
                                         {"TREND_FOLLOWING_FAST", 2.5, 0.25, 0.3}}),
              "SLEEVE_CONFIG book=BASE_PORTFOLIO sleeve=TREND_FOLLOWING idm=2.500000 "
              "risk_target=0.200000 allocation=0.700000 sleeve=TREND_FOLLOWING_FAST idm=2.500000 "
              "risk_target=0.250000 allocation=0.300000");
    EXPECT_EQ(resolved_sleeves_log_line("EQUITY_PORTFOLIO",
                                        {{"EQUITY_MEAN_REVERSION", std::nullopt, 0.15, 1.0}}),
              "SLEEVE_CONFIG book=EQUITY_PORTFOLIO sleeve=EQUITY_MEAN_REVERSION idm=none "
              "risk_target=0.150000 allocation=1.000000");
}
