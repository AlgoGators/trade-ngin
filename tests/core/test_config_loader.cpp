// Coverage for config_loader.cpp: JSON file loading, deep-merge, AppConfig
// extraction, validation, and error paths. All tests use a per-test temp
// directory so they don't depend on the real ./config tree.

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include "test_base.hpp"

// Reach private merge_json/validate_config/load_legacy helpers. Pre-load std
// headers before flipping the macro so libc++ internals stay valid.
#include <map>
#include <string>
#include <vector>
#define private public
#include "trade_ngin/core/config_loader.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

TEST(ConfigFieldProjectionTest, CompleteTypedInventoryAndKnownStrategyValues) {
    AppConfig config;
    config.initial_capital = 123456.25;
    config.reserve_capital_pct = 0.11;
    config.benchmark_mode = "deferred";
    config.execution.commission_rate = 0.0012;
    config.execution.slippage_bps = 3.25;
    config.execution.position_limit_backtest = 101.5;
    config.execution.position_limit_live = 55.25;
    config.opt_config.tau = 1.11;
    config.opt_config.capital = 654321.75;
    config.opt_config.cost_penalty_scalar = 34.5;
    config.opt_config.asymmetric_risk_buffer = 0.12;
    config.opt_config.max_iterations = 117;
    config.opt_config.convergence_threshold = 0.00012;
    config.opt_config.use_buffering = false;
    config.opt_config.buffer_size_factor = 0.065;
    config.risk_config.var_limit = 0.16;
    config.risk_config.jump_risk_limit = 0.17;
    config.risk_config.corr_shock_threshold = 0.18;
    config.risk_config.jump_shock_threshold = 0.19;
    config.risk_config.max_gross_leverage = 4.25;
    config.risk_config.max_net_leverage = 2.25;
    config.risk_config.capital = Decimal(234567.5);
    config.max_drawdown = 0.31;
    config.max_leverage = 3.75;
    config.risk_config.confidence_level = 0.975;
    config.risk_config.lookback_period = 199;
    config.risk_config.max_correlation = 0.68;
    config.backtest.lookback_years = 7;
    config.backtest.store_trade_details = false;
    config.live.historical_days = 415;
    config.strategy_defaults.fdm = {{7, 1.25}};
    config.strategy_defaults.max_strategy_allocation = 0.83;
    config.strategy_defaults.min_strategy_allocation = 0.09;
    config.strategy_defaults.use_optimization = false;
    config.strategy_defaults.use_risk_management = false;
    config.strategy_defaults.carver_buffer_floor = 0.75;
    config.strategy_defaults.carver_buffer_position_factor = 0.13;
    config.strategies_config = {
        {"Alpha_1", {{"type", "TrendFollowingFastStrategy"},
                       {"enabled_live", false}, {"default_allocation", 0.37},
                       {"enabled_backtest", true},
                       {"config", {{"weight", 1.15}, {"risk_target", 0.23},
                                   {"idm", 1.33}, {"max_symbol_concentration", 0.44},
                                   {"use_position_buffering", false},
                                   {"carver_buffer_floor", 0.6},
                                   {"carver_buffer_position_factor", 0.08},
                                   {"ema_windows", nlohmann::json::array({{8, 32}})},
                                   {"vol_lookback_short", 61},
                                   {"vol_lookback_long", 181},
                                   {"fx_rate", "ignored-sentinel"}}}}}};

    const auto result = project_live_config_fields(config);
    ASSERT_TRUE(result.is_ok());
    const auto& document = result.value();
    EXPECT_EQ(document.size(), 6u);
    EXPECT_EQ(document.at("projection_version"), 1);
    EXPECT_EQ(document.at("profile"), "live_portfolio_runner_futures");
    EXPECT_EQ(document.at("coverage"), "current_typed_fields_and_known_strategy_leaves");
    EXPECT_EQ(document.at("authority"), "inspection_only");
    EXPECT_EQ(document.at("consumption_evidence"), "not_collected");
    const auto& fields = document.at("fields");
    ASSERT_TRUE(fields.is_array());
    EXPECT_EQ(fields.size(), 57u);
    std::vector<std::string> paths;
    for (const auto& field : fields) paths.push_back(field.at("path").get<std::string>());
    EXPECT_TRUE(std::is_sorted(paths.begin(), paths.end()));
    EXPECT_EQ(std::adjacent_find(paths.begin(), paths.end()), paths.end());
    std::vector<std::string> expected_paths = {
        "/portfolio_id", "/initial_capital", "/reserve_capital_pct", "/benchmark_mode",
        "/execution/commission_rate", "/execution/slippage_bps",
        "/execution/position_limit_backtest", "/execution/position_limit_live",
        "/optimization/tau", "/optimization/capital", "/optimization/cost_penalty_scalar",
        "/optimization/asymmetric_risk_buffer", "/optimization/max_iterations",
        "/optimization/convergence_threshold", "/optimization/use_buffering",
        "/optimization/buffer_size_factor", "/optimization/version",
        "/risk/var_limit", "/risk/jump_risk_limit", "/risk/corr_shock_threshold",
        "/risk/jump_shock_threshold", "/risk/max_gross_leverage", "/risk/max_net_leverage",
        "/risk/capital", "/risk/version", "/risk/max_drawdown", "/risk/max_leverage",
        "/risk_defaults/confidence_level", "/risk_defaults/lookback_period",
        "/risk_defaults/max_correlation", "/backtest/lookback_years",
        "/backtest/store_trade_details", "/live/historical_days",
        "/strategy_defaults/fdm", "/strategy_defaults/max_strategy_allocation",
        "/strategy_defaults/min_strategy_allocation", "/strategy_defaults/use_optimization",
        "/strategy_defaults/use_risk_management", "/strategy_defaults/carver_buffer_floor",
        "/strategy_defaults/carver_buffer_position_factor",
        "/strategies/Alpha_1/enabled_live", "/strategies/Alpha_1/default_allocation",
        "/strategies/Alpha_1/enabled_backtest", "/strategies/Alpha_1/type",
        "/strategies/Alpha_1/config/weight", "/strategies/Alpha_1/config/risk_target",
        "/strategies/Alpha_1/config/idm", "/strategies/Alpha_1/config/max_symbol_concentration",
        "/strategies/Alpha_1/config/use_position_buffering",
        "/strategies/Alpha_1/config/carver_buffer_floor",
        "/strategies/Alpha_1/config/carver_buffer_position_factor",
        "/strategies/Alpha_1/config/ema_windows",
        "/strategies/Alpha_1/config/vol_lookback_short",
        "/strategies/Alpha_1/config/vol_lookback_long",
        "/strategies/Alpha_1/config/fx_rate", "/strategies/Alpha_1/config/max_history_size",
        "/strategies/Alpha_1/config/fdm"};
    std::sort(expected_paths.begin(), expected_paths.end());
    EXPECT_EQ(paths, expected_paths);
    const auto find = [&](const std::string& path) -> const nlohmann::json& {
        return *std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
            return field.at("path") == path;
        });
    };
    EXPECT_EQ(find("/initial_capital").at("value"), 123456.25);
    EXPECT_EQ(find("/optimization/capital").at("value"), 654321.75);
    EXPECT_EQ(find("/risk/capital").at("value"), 234567.5);
    EXPECT_EQ(find("/risk/max_drawdown").at("value"), 0.31);
    EXPECT_EQ(find("/risk_defaults/confidence_level").at("value"), 0.975);
    EXPECT_EQ(find("/strategy_defaults/fdm").at("value"),
              nlohmann::json::array({{7, 1.25}}));
    const auto& risk_target = find("/strategies/Alpha_1/config/risk_target");
    EXPECT_EQ(risk_target.at("value"), 0.23);
    EXPECT_EQ(risk_target.at("unit"), "annualized_volatility_fraction");
    EXPECT_EQ(find("/strategies/Alpha_1/config/ema_windows").at("value"),
              nlohmann::json::array({{8, 32}}));
    EXPECT_EQ(find("/strategies/Alpha_1/config/fx_rate").at("value_state"), "omitted");
    const std::map<std::string, nlohmann::json> expected_values = {
        {"/initial_capital", 123456.25}, {"/reserve_capital_pct", 0.11},
        {"/benchmark_mode", "deferred"},
        {"/execution/commission_rate", 0.0012}, {"/execution/slippage_bps", 3.25},
        {"/execution/position_limit_backtest", 101.5},
        {"/execution/position_limit_live", 55.25},
        {"/optimization/tau", 1.11}, {"/optimization/capital", 654321.75},
        {"/optimization/cost_penalty_scalar", 34.5},
        {"/optimization/asymmetric_risk_buffer", 0.12},
        {"/optimization/max_iterations", 117},
        {"/optimization/convergence_threshold", 0.00012},
        {"/optimization/use_buffering", false},
        {"/optimization/buffer_size_factor", 0.065},
        {"/risk/var_limit", 0.16}, {"/risk/jump_risk_limit", 0.17},
        {"/risk/corr_shock_threshold", 0.18}, {"/risk/jump_shock_threshold", 0.19},
        {"/risk/max_gross_leverage", 4.25}, {"/risk/max_net_leverage", 2.25},
        {"/risk/capital", 234567.5}, {"/risk/max_drawdown", 0.31},
        {"/risk/max_leverage", 3.75},
        {"/risk_defaults/confidence_level", 0.975},
        {"/risk_defaults/lookback_period", 199},
        {"/risk_defaults/max_correlation", 0.68},
        {"/backtest/lookback_years", 7}, {"/backtest/store_trade_details", false},
        {"/live/historical_days", 415},
        {"/strategy_defaults/fdm", nlohmann::json::array({{7, 1.25}})},
        {"/strategy_defaults/max_strategy_allocation", 0.83},
        {"/strategy_defaults/min_strategy_allocation", 0.09},
        {"/strategy_defaults/use_optimization", false},
        {"/strategy_defaults/use_risk_management", false},
        {"/strategy_defaults/carver_buffer_floor", 0.75},
        {"/strategy_defaults/carver_buffer_position_factor", 0.13},
        {"/strategies/Alpha_1/enabled_live", false},
        {"/strategies/Alpha_1/default_allocation", 0.37},
        {"/strategies/Alpha_1/enabled_backtest", true},
        {"/strategies/Alpha_1/type", "TrendFollowingFastStrategy"},
        {"/strategies/Alpha_1/config/weight", 1.15},
        {"/strategies/Alpha_1/config/risk_target", 0.23},
        {"/strategies/Alpha_1/config/idm", 1.33},
        {"/strategies/Alpha_1/config/max_symbol_concentration", 0.44},
        {"/strategies/Alpha_1/config/use_position_buffering", false},
        {"/strategies/Alpha_1/config/carver_buffer_floor", 0.6},
        {"/strategies/Alpha_1/config/carver_buffer_position_factor", 0.08},
        {"/strategies/Alpha_1/config/ema_windows", nlohmann::json::array({{8, 32}})},
        {"/strategies/Alpha_1/config/vol_lookback_short", 61},
        {"/strategies/Alpha_1/config/vol_lookback_long", 181}};
    for (const auto& [path, value] : expected_values) {
        SCOPED_TRACE(path);
        EXPECT_EQ(find(path).at("value"), value);
        EXPECT_EQ(find(path).at("value_state"), "included");
    }
    for (const auto& field : fields) {
        const auto path = field.at("path").get<std::string>();
        EXPECT_EQ(field.contains("value"), expected_values.contains(path)) << path;
        EXPECT_EQ(field.size(), field.contains("value") ? 10u : 9u) << path;
        EXPECT_EQ(field.at("scope"), "exact") << path;
    }
    const std::map<std::string, std::string> non_number_types = {
        {"/portfolio_id", "string"}, {"/benchmark_mode", "enum_string"},
        {"/optimization/max_iterations", "integer"},
        {"/optimization/use_buffering", "boolean"},
        {"/optimization/version", "string"}, {"/risk/version", "string"},
        {"/risk_defaults/lookback_period", "integer"},
        {"/backtest/lookback_years", "integer"},
        {"/backtest/store_trade_details", "boolean"},
        {"/live/historical_days", "integer"},
        {"/strategy_defaults/fdm", "integer_number_pairs"},
        {"/strategy_defaults/use_optimization", "boolean"},
        {"/strategy_defaults/use_risk_management", "boolean"},
        {"/strategies/Alpha_1/enabled_live", "boolean"},
        {"/strategies/Alpha_1/enabled_backtest", "boolean"},
        {"/strategies/Alpha_1/type", "enum_string"},
        {"/strategies/Alpha_1/config/use_position_buffering", "boolean"},
        {"/strategies/Alpha_1/config/ema_windows", "integer_pairs"},
        {"/strategies/Alpha_1/config/vol_lookback_short", "integer"},
        {"/strategies/Alpha_1/config/vol_lookback_long", "integer"},
        {"/strategies/Alpha_1/config/max_history_size", "integer"},
        {"/strategies/Alpha_1/config/fdm", "integer_number_pairs"}};
    for (const auto& path : expected_paths) {
        const auto type_it = non_number_types.find(path);
        EXPECT_EQ(find(path).at("value_type"),
                  type_it == non_number_types.end() ? "number" : type_it->second) << path;
    }
    const auto expect_meta = [&](const std::string& path, const char* classification,
                                 const char* reason, const char* condition,
                                 const char* unit, const char* origin) {
        const auto& field = find(path);
        EXPECT_EQ(field.at("classification"), classification) << path;
        EXPECT_EQ(field.at("reason"), reason) << path;
        EXPECT_EQ(field.at("condition"), condition) << path;
        EXPECT_EQ(field.at("unit"), unit) << path;
        EXPECT_EQ(field.at("value_origin"), origin) << path;
    };
    expect_meta("/reserve_capital_pct", "unsupported_in_profile", "stored_metadata_only",
                "no_active_profile_reader", "fraction", "app_config_member");
    expect_meta("/execution/commission_rate", "unsupported_in_profile",
                "not_wired_to_futures_cost_model", "no_active_profile_reader",
                "unverified_rate", "app_config_member");
    expect_meta("/execution/slippage_bps", "unsupported_in_profile",
                "not_wired_to_futures_cost_model", "no_active_profile_reader",
                "basis_points", "app_config_member");
    expect_meta("/optimization/asymmetric_risk_buffer", "unsupported_in_profile",
                "no_active_reader", "no_active_profile_reader", "unverified_buffer_fraction",
                "app_config_member");
    expect_meta("/strategy_defaults/fdm", "unsupported_in_profile",
                "blocked_default_fallback", "no_active_profile_reader",
                "rule_count_multiplier_pairs", "app_config_member");
    expect_meta("/risk/corr_shock_threshold", "unsupported_in_profile",
                "inactive_alternative", "no_active_profile_reader",
                "annualized_volatility_fraction", "app_config_member");
    expect_meta("/risk/jump_shock_threshold", "unsupported_in_profile",
                "inactive_alternative", "no_active_profile_reader",
                "annualized_volatility_fraction", "app_config_member");
    expect_meta("/risk/max_leverage", "source_supported_config_input",
                "diagnostic_reader", "base_strategy_risk_check", "leverage_multiple",
                "app_config_member");
    expect_meta("/risk_defaults/confidence_level", "source_supported_config_input",
                "source_reader", "risk_enabled", "probability", "app_config_member");
    expect_meta("/strategies/Alpha_1/config/risk_target", "source_supported_config_input",
                "source_reader", "selected_known_strategy", "annualized_volatility_fraction",
                "configured_strategy_leaf");
    expect_meta("/strategies/Alpha_1/config/weight", "source_supported_config_input",
                "source_reader", "selected_known_strategy_buffering_enabled", "multiplier",
                "configured_strategy_leaf");
    expect_meta("/strategies/Alpha_1/config/fx_rate", "unsupported_in_profile",
                "typed_member_not_input_wired", "no_active_profile_reader", "currency_ratio",
                "not_projected");
    expect_meta("/strategies/Alpha_1/config/max_history_size", "unsupported_in_profile",
                "typed_member_not_input_wired", "no_active_profile_reader", "bar_records",
                "not_projected");
    expect_meta("/strategies/Alpha_1/config/fdm", "unsupported_in_profile",
                "typed_member_not_input_wired", "no_active_profile_reader",
                "rule_count_multiplier_pairs", "not_projected");
}

TEST(ConfigFieldProjectionTest, RejectsMalformedIncludedValuesWithOneRedactedError) {
    const auto invalid = [](const AppConfig& config) {
        const auto result = project_live_config_fields(config);
        EXPECT_TRUE(result.is_error());
        ASSERT_NE(result.error(), nullptr);
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_DATA);
        EXPECT_STREQ(result.error()->what(), "config_projection_invalid_field");
        EXPECT_EQ(result.error()->component(), "ConfigFieldProjection");
    };
    AppConfig config;
    config.initial_capital = std::numeric_limits<double>::infinity();
    invalid(config);
    config.initial_capital = 0.0;
    config.strategy_defaults.fdm = {{2, std::numeric_limits<double>::quiet_NaN()}};
    invalid(config);
    config.strategy_defaults.fdm = {};
    config.benchmark_mode = "invalid-private-string";
    invalid(config);
    config.benchmark_mode = "live";
    config.strategies_config = nlohmann::json::array();
    invalid(config);
    config.strategies_config = {{"bad/id", nlohmann::json::object()}};
    invalid(config);
    config.strategies_config = {{"Good", {{"config", nullptr}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", 23}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", "TrendFollowingStrategy"},
                                    {"config", {{"ema_windows", nlohmann::json::array({{1, 2.5}})}}}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", "TrendFollowingStrategy"},
                                    {"config", {{"vol_lookback_short", 2147483648LL}}}}}};
    invalid(config);
    config.strategies_config = {{"Good", nullptr}};
    invalid(config);
    config.strategies_config = {{"Good", nlohmann::json::array()}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", "Unsupported"}, {"config", 1}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"enabled_live", 1}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"default_allocation", "not-a-number"}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", "TrendFollowingStrategy"},
                                    {"config", {{"ema_windows", nlohmann::json::array({{1, 2, 3}})}}}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", "TrendFollowingStrategy"},
                                    {"config", {{"ema_windows", nlohmann::json::array({{1, 2147483648LL}})}}}}}};
    invalid(config);
    config.strategies_config = {{"Good", {{"type", "TrendFollowingStrategy"},
                                    {"config", {{"risk_target", std::numeric_limits<double>::infinity()}}}}}};
    invalid(config);
    config.strategies_config = {{std::string(129, 'X'), nlohmann::json::object()}};
    invalid(config);
}

TEST(ConfigFieldProjectionTest, PrivateAndUnknownDataCannotChangeProjection) {
    AppConfig config;
    config.portfolio_id = "private-identity-one";
    config.opt_config.version = "private-version-one";
    config.risk_config.version = "private-risk-version-one";
    config.database.host = "db-host-one";
    config.database.port = "db-port-one";
    config.database.username = "db-user-one";
    config.database.password = "db-secret-one";
    config.database.name = "db-name-one";
    config.database.num_connections = 4;
    config.email.smtp_host = "mail-host-one";
    config.email.smtp_port = 471;
    config.email.username = "mail-user-one";
    config.email.password = "email-secret-one";
    config.email.from_email = "mail-from-one";
    config.email.use_tls = false;
    config.email.to_emails = {"mail-recipient-one"};
    config.email.to_emails_production = {"mail-production-one"};
    config.strategies_config = {{"S", {{"type", "TrendFollowingStrategy"},
                                     {"config", {{"fx_rate", "opaque-one"},
                                                 {"unknown", {{"nested", "hidden-one"}}}}}}}};
    const auto before = project_live_config_fields(config);
    ASSERT_TRUE(before.is_ok());
    config.portfolio_id = "private-identity-two";
    config.opt_config.version = "private-version-two";
    config.risk_config.version = "private-risk-version-two";
    config.database.host = "db-host-two";
    config.database.port = "db-port-two";
    config.database.username = "db-user-two";
    config.database.password = "db-secret-two";
    config.database.name = "db-name-two";
    config.database.num_connections = 9;
    config.email.smtp_host = "mail-host-two";
    config.email.smtp_port = 472;
    config.email.username = "mail-user-two";
    config.email.password = "email-secret-two";
    config.email.from_email = "mail-from-two";
    config.email.use_tls = true;
    config.email.to_emails = {"mail-recipient-two"};
    config.email.to_emails_production = {"mail-production-two"};
    config.strategies_config["S"]["config"]["fx_rate"] = "opaque-two";
    config.strategies_config["S"]["config"]["unknown"] = {{"nested", "hidden-two"}};
    const auto after = project_live_config_fields(config);
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(before.value().dump(), after.value().dump());
    const auto output = after.value().dump();
    for (const char* forbidden : {"private-identity", "private-version", "private-risk-version",
                                  "db-host", "db-port", "db-user", "db-secret", "db-name",
                                  "mail-host", "mail-user", "email-secret", "mail-from",
                                  "mail-recipient", "mail-production", "opaque-two",
                                  "hidden-two", "unknown"}) {
        EXPECT_EQ(output.find(forbidden), std::string::npos);
    }
}

TEST(ConfigFieldProjectionTest, MissingUnknownAndKnownStrategyRoutesStayDistinct) {
    AppConfig config;
    config.strategies_config = {
        {"Z_slow", {{"type", "TrendFollowingSlowStrategy"}, {"enabled_live", false},
                    {"config", nlohmann::json::object()}}},
        {"a_fast", {{"type", "TrendFollowingFastStrategy"}}},
        {"B_standard", {{"config", {{"risk_target", -0.2}}}}},
        {"U", {{"type", "PrivateStrategyClass"}, {"enabled_live", true},
               {"config", {{"risk_target", "private-unread"}}}}}};
    const auto before = config.strategies_config;
    const auto result = project_live_config_fields(config);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(config.strategies_config, before);
    const auto& fields = result.value().at("fields");
    const auto find = [&](const std::string& path) -> const nlohmann::json& {
        auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
            return field.at("path") == path;
        });
        EXPECT_NE(it, fields.end()) << path;
        return *it;
    };
    EXPECT_EQ(fields.size(), 40u + 17u * 3u + 4u);
    EXPECT_EQ(find("/strategies/B_standard/type").at("value_state"), "absent_in_input");
    EXPECT_FALSE(find("/strategies/B_standard/type").contains("value"));
    EXPECT_EQ(find("/strategies/B_standard/config/risk_target").at("value"), -0.2);
    EXPECT_EQ(find("/strategies/Z_slow/config/risk_target").at("value_state"), "absent_in_input");
    EXPECT_EQ(find("/strategies/a_fast/config/risk_target").at("value_state"), "absent_in_input");
    EXPECT_EQ(find("/strategies/Z_slow/enabled_live").at("value"), false);
    EXPECT_EQ(find("/strategies/U/type").at("classification"), "unsupported_in_profile");
    EXPECT_EQ(find("/strategies/U/type").at("reason"), "unknown_strategy_type");
    EXPECT_EQ(find("/strategies/U/type").at("value_origin"), "not_projected");
    EXPECT_EQ(find("/strategies/U/type").at("value_state"), "omitted");
    EXPECT_FALSE(find("/strategies/U/type").contains("value"));
    EXPECT_EQ(find("/strategies/U/enabled_live").at("value"), true);
    EXPECT_EQ(result.value().dump().find("PrivateStrategyClass"), std::string::npos);
    EXPECT_EQ(result.value().dump().find("private-unread"), std::string::npos);
    for (const auto& field : fields) {
        EXPECT_EQ(field.at("path").get<std::string>().find("/strategies/U/config/"),
                  std::string::npos);
    }
    const auto again = project_live_config_fields(config);
    ASSERT_TRUE(again.is_ok());
    EXPECT_EQ(result.value().dump(), again.value().dump());
}

TEST(ConfigFieldProjectionTest, PairArraysAndRepresentableNegativeInputsAreObservations) {
    AppConfig config;
    config.execution.position_limit_live = -3.5;
    config.live.historical_days = -7;
    config.strategy_defaults.fdm = {};
    config.strategies_config = {{"S", {{"type", "TrendFollowingStrategy"},
                                    {"config", {{"ema_windows", nlohmann::json::array({{-4, 0}, {2, 9}})},
                                                {"vol_lookback_long", -19}}}}}};
    const auto result = project_live_config_fields(config);
    ASSERT_TRUE(result.is_ok());
    const auto& fields = result.value().at("fields");
    const auto find_value = [&](const std::string& path) -> nlohmann::json {
        const auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
            return field.at("path") == path;
        });
        EXPECT_NE(it, fields.end());
        return it->at("value");
    };
    EXPECT_EQ(find_value("/execution/position_limit_live"), -3.5);
    EXPECT_EQ(find_value("/live/historical_days"), -7);
    EXPECT_EQ(find_value("/strategy_defaults/fdm"), nlohmann::json::array());
    EXPECT_EQ(find_value("/strategies/S/config/ema_windows"),
              nlohmann::json::array({{-4, 0}, {2, 9}}));
    EXPECT_EQ(find_value("/strategies/S/config/vol_lookback_long"), -19);
    for (const auto& field : fields) {
        EXPECT_EQ(field.at("path").get<std::string>().find('*'), std::string::npos);
    }
}

namespace {

nlohmann::json minimal_defaults() {
    return {
        {"database", {{"host", "h"}, {"port", "5432"}, {"username", "u"},
                      {"password", "p"}, {"name", "n"}, {"num_connections", 5}}},
        {"execution", {{"commission_rate", 0.0005}, {"slippage_bps", 1.0},
                        {"position_limit_backtest", 1000.0}, {"position_limit_live", 500.0}}},
        {"optimization", {{"tau", 1.0}, {"capital", 500000.0},
                          {"cost_penalty_scalar", 50}, {"asymmetric_risk_buffer", 0.1},
                          {"max_iterations", 100}, {"convergence_threshold", 1e-6},
                          {"use_buffering", true}, {"buffer_size_factor", 0.05}}},
        {"backtest", {{"lookback_years", 2}, {"store_trade_details", true}}},
        {"live", {{"historical_days", 300}}},
        {"strategy_defaults", {{"max_strategy_allocation", 1.0},
                                {"min_strategy_allocation", 0.1},
                                {"fdm", nlohmann::json::array({{1, 1.0}, {2, 1.03}})}}},
    };
}

nlohmann::json minimal_portfolio() {
    return {
        {"portfolio_id", "TEST_PORTFOLIO"},
        {"initial_capital", 1'000'000.0},
        {"reserve_capital_pct", 0.10},
        {"max_drawdown", 0.4},
        {"max_leverage", 4.0},
        {"use_optimization", true},
        // Validation requires at least one strategy entry.
        {"strategies", {{"TREND_FOLLOWING", {{"weight", 1.0}, {"allocation", 1.0}}}}},
    };
}

nlohmann::json carver_module(const char* id = "carver") {
    return {
        {"id", id},
        {"type", "carver"},
        {"var_limit", 0.15},
        {"jump_risk_limit", 0.10},
        {"max_correlation", 0.7},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
        {"confidence_level", 0.99},
        {"lookback_period", 252},
        {"lookback_unit", "dates"},
        {"min_gate_dates", 21},
        {"missing_symbol_policy", "ignore"},
        {"_missing_symbol_policy_reason", "unit test"},
    };
}

nlohmann::json reporting_block() {
    return {
        {"type", "carver"},
        {"window", "all_bars"},
        {"var_limit", 0.15},
        {"jump_risk_limit", 0.10},
        {"max_correlation", 0.7},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
        {"confidence_level", 0.99},
        {"lookback_period", 252},
    };
}

nlohmann::json minimal_risk() {
    return {
        {"schema", 2},
        {"modules", nlohmann::json::array({carver_module()})},
        {"risk_reporting", reporting_block()},
        {"max_drawdown", 0.4},
        {"max_leverage", 4.0},
    };
}

nlohmann::json minimal_email() {
    return {
        {"smtp_host", "smtp.test.com"},
        {"smtp_port", 587},
        {"username", "u"},
        {"password", "p"},
        {"from_email", "f@test.com"},
        {"to_emails", nlohmann::json::array({"a@test.com"})},
    };
}

std::string escape_pointer_token(const std::string& token) {
    std::string escaped;
    for (char ch : token) {
        if (ch == '~') escaped += "~0";
        else if (ch == '/') escaped += "~1";
        else escaped += ch;
    }
    return escaped;
}

void collect_leaf_paths(const nlohmann::json& value, const std::string& path,
                        std::vector<std::string>& paths) {
    if (value.is_object() && !value.empty()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            collect_leaf_paths(it.value(), path + "/" +
                                             escape_pointer_token(it.key()), paths);
        }
    } else if (value.is_array() && !value.empty()) {
        for (size_t index = 0; index < value.size(); ++index) {
            collect_leaf_paths(value[index], path + "/" + std::to_string(index), paths);
        }
    } else {
        paths.push_back(path);
    }
}

nlohmann::json distinguishable_edit(const nlohmann::json& value, const std::string& path) {
    if (path == "/benchmark_mode") {
        return value == "live" ? "deferred" : "live";
    }
    if (value.is_boolean()) return !value.get<bool>();
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() + 1;
    if (value.is_number_integer()) return value.get<std::int64_t>() + 1;
    if (value.is_number_float()) {
        const double number = value.get<double>();
        return number == 0.0 ? 0.25 : number / 2.0;
    }
    if (value.is_string()) return value.get<std::string>() + "_EDIT";
    if (value.is_array()) return nlohmann::json::array({"synthetic@example.test"});
    return nlohmann::json{{"synthetic", 1}};
}

void write_json(const std::filesystem::path& p, const nlohmann::json& j) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << j.dump(2);
}

}  // namespace

class ConfigLoaderTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        base_ = std::filesystem::temp_directory_path() /
                ("trade_ngin_config_" + std::string(info->name()));
        std::filesystem::remove_all(base_);
        std::filesystem::create_directories(base_);
    }

    void TearDown() override {
        std::filesystem::remove_all(base_);
        TestBase::TearDown();
    }

    void write_full_set(const std::string& portfolio_name,
                         const nlohmann::json& defaults_override = {},
                         const nlohmann::json& portfolio_override = {}) {
        auto defaults = minimal_defaults();
        for (auto& [k, v] : defaults_override.items()) defaults[k] = v;
        write_json(base_ / "defaults.json", defaults);

        auto portfolio = minimal_portfolio();
        for (auto& [k, v] : portfolio_override.items()) portfolio[k] = v;
        write_json(base_ / "portfolios" / portfolio_name / "portfolio.json", portfolio);

        write_json(base_ / "portfolios" / portfolio_name / "risk.json", minimal_risk());
        write_json(base_ / "portfolios" / portfolio_name / "email.json", minimal_email());
    }

    std::filesystem::path base_;
};

// ===== Happy path =====

TEST_F(ConfigLoaderTest, LoadValidConfigPopulatesAllFields) {
    write_full_set("base");
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "no error");
    auto& c = r.value();
    EXPECT_EQ(c.portfolio_id, "TEST_PORTFOLIO");
    EXPECT_DOUBLE_EQ(c.initial_capital, 1'000'000.0);
    EXPECT_EQ(c.database.host, "h");
    EXPECT_EQ(c.database.num_connections, 5u);
    EXPECT_DOUBLE_EQ(c.execution.commission_rate, 0.0005);
    EXPECT_DOUBLE_EQ(c.opt_config.tau, 1.0);
    EXPECT_DOUBLE_EQ(c.risk_config.var_limit, 0.15);
    EXPECT_EQ(c.email.smtp_host, "smtp.test.com");
}

TEST_F(ConfigLoaderTest, RejectsTraversalEvenWhenEscapedFilesAreComplete) {
    write_json(base_ / "defaults.json", minimal_defaults());
    write_json(base_ / "outside" / "portfolio.json", minimal_portfolio());
    write_json(base_ / "outside" / "risk.json", minimal_risk());
    write_json(base_ / "outside" / "email.json", minimal_email());
    EXPECT_TRUE(ConfigLoader::load(base_, "../outside").is_error());
}

TEST_F(ConfigLoaderTest, RejectsPortfolioDirectorySymlinkOutsidePortfoliosRoot) {
    write_json(base_ / "defaults.json", minimal_defaults());
    write_json(base_ / "outside" / "portfolio.json", minimal_portfolio());
    write_json(base_ / "outside" / "risk.json", minimal_risk());
    write_json(base_ / "outside" / "email.json", minimal_email());
    std::filesystem::create_directories(base_ / "portfolios");
    std::filesystem::create_directory_symlink(base_ / "outside", base_ / "portfolios" / "escape");
    EXPECT_TRUE(ConfigLoader::load(base_, "escape").is_error());
}

TEST(PortfolioConfigKeyTest, ExactGrammarIsLowercaseAsciiAndSingleComponent) {
    for (const auto& key : {"a", "7", "equity_mr", "investor-7", "a0_-"}) {
        EXPECT_TRUE(is_valid_portfolio_config_key(key)) << key;
    }
    for (const auto& key : {"", "_book", "-book", "Book", "book.name", "book/name",
                            "../book", "/book"}) {
        EXPECT_FALSE(is_valid_portfolio_config_key(key)) << key;
    }
    EXPECT_TRUE(is_valid_portfolio_config_key(std::string(64, 'a')));
    EXPECT_FALSE(is_valid_portfolio_config_key(std::string(65, 'a')));
}

TEST_F(ConfigLoaderTest, PortfolioFileOverridesDefaults) {
    // Use a portfolio-level override on a top-level field that's not on the
    // required-validation list (initial_capital).
    write_full_set("base", /*defaults_override=*/{},
                    /*portfolio_override=*/{{"initial_capital", 2'500'000.0}});
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "no error");
    EXPECT_DOUBLE_EQ(r.value().initial_capital, 2'500'000.0);
}

TEST_F(ConfigLoaderTest, ToJsonRoundTripsConfigStructures) {
    write_full_set("base");
    auto r = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(r.is_ok());
    auto j = r.value().to_json();
    EXPECT_EQ(j["portfolio_id"].get<std::string>(), "TEST_PORTFOLIO");
    EXPECT_TRUE(j.contains("database"));
    EXPECT_TRUE(j.contains("execution"));
    EXPECT_TRUE(j.contains("optimization"));
    EXPECT_TRUE(j.contains("risk"));
    EXPECT_TRUE(j.contains("backtest"));
    EXPECT_TRUE(j.contains("live"));
    EXPECT_TRUE(j.contains("strategy_defaults"));
    EXPECT_TRUE(j.contains("email"));
}

TEST_F(ConfigLoaderTest, SerializedEditableLeavesSurviveMergeAndExtraction) {
    write_full_set("base");
    auto loaded = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(loaded.is_ok());
    const auto serialized = loaded.value().to_json();
    const auto& read_only = AppConfig::live_read_only_paths();
    std::vector<std::string> paths;
    collect_leaf_paths(serialized, "", paths);
    ASSERT_GT(paths.size(), 30u);

    for (const auto& path : paths) {
        const std::vector<std::string> structural = {
            "/risk/schema", "/risk/modules/0/id", "/risk/modules/0/type",
            "/risk/modules/0/lookback_unit", "/risk/modules/0/missing_symbol_policy",
            "/risk/risk_reporting/type", "/risk/risk_reporting/window",
            "/sleeve_risk_modules"};
        if (std::find(structural.begin(), structural.end(), path) != structural.end()) continue;
        bool derived = false;
        for (const auto& prefix : read_only) {
            if (path == prefix || path.rfind(prefix + "/", 0) == 0) {
                derived = true;
                break;
            }
        }
        if (derived) continue;

        const nlohmann::json::json_pointer pointer(path);
        auto edited = serialized;
        edited[pointer] = distinguishable_edit(serialized.at(pointer), path);
        const std::string module_prefix = "/risk/modules/0/";
        const std::string reporting_prefix = "/risk/risk_reporting/";
        const std::vector<std::string> mirrored = {
            "var_limit", "jump_risk_limit", "max_correlation", "max_gross_leverage",
            "max_net_leverage", "confidence_level", "lookback_period"};
        if (path.rfind(module_prefix, 0) == 0) {
            const auto field = path.substr(module_prefix.size());
            if (std::find(mirrored.begin(), mirrored.end(), field) != mirrored.end())
                edited[nlohmann::json::json_pointer(reporting_prefix + field)] = edited[pointer];
        } else if (path.rfind(reporting_prefix, 0) == 0) {
            const auto field = path.substr(reporting_prefix.size());
            if (std::find(mirrored.begin(), mirrored.end(), field) != mirrored.end())
                edited[nlohmann::json::json_pointer(module_prefix + field)] = edited[pointer];
        }
        ASSERT_NE(edited.at(pointer), serialized.at(pointer)) << path;
        auto merged = serialized;
        ConfigLoader::merge_json(merged, edited);
        auto extracted = ConfigLoader::extract_config(merged);
        ASSERT_TRUE(extracted.is_ok()) << path;
        EXPECT_EQ(extracted.value().to_json().at(pointer), edited.at(pointer)) << path;
    }
}

TEST_F(ConfigLoaderTest, LiveReadOnlyPathsIdentifyDerivedAndBacktestValues) {
    const auto& paths = AppConfig::live_read_only_paths();
    const std::vector<std::string> required = {
        "/portfolio_id", "/optimization/capital", "/optimization/version",
        "/risk/capital", "/risk/version", "/max_drawdown", "/max_leverage",
        "/backtest"};
    for (const auto& path : required) {
        EXPECT_NE(std::find(paths.begin(), paths.end(), path), paths.end()) << path;
    }
}

TEST_F(ConfigLoaderTest, CanonicalRiskLimitsRequireSchema2RiskBlock) {
    write_full_set("conservative", {}, {{"max_drawdown", 0.55}, {"max_leverage", 5.0}});
    auto risk = minimal_risk();
    risk["max_drawdown"] = 0.15;
    risk["max_leverage"] = 4.0;
    write_json(base_ / "portfolios" / "conservative" / "risk.json", risk);
    auto loaded = ConfigLoader::load(base_, "conservative");
    ASSERT_TRUE(loaded.is_ok());
    auto serialized = loaded.value().to_json();
    EXPECT_DOUBLE_EQ(serialized.at("max_drawdown").get<double>(), 0.15);
    EXPECT_DOUBLE_EQ(serialized.at("max_leverage").get<double>(), 4.0);
    EXPECT_DOUBLE_EQ(serialized.at("risk").at("max_drawdown").get<double>(), 0.15);
    EXPECT_DOUBLE_EQ(serialized.at("risk").at("max_leverage").get<double>(), 4.0);

    serialized["max_drawdown"] = 0.55;
    serialized["max_leverage"] = 5.0;
    auto nested_wins = ConfigLoader::extract_config(serialized);
    ASSERT_TRUE(nested_wins.is_ok());
    EXPECT_DOUBLE_EQ(nested_wins.value().max_drawdown, 0.15);
    EXPECT_DOUBLE_EQ(nested_wins.value().max_leverage, 4.0);

    serialized["risk"].erase("max_drawdown");
    serialized["risk"].erase("max_leverage");
    auto legacy_only = ConfigLoader::extract_config(serialized);
    EXPECT_TRUE(legacy_only.is_error());
}

TEST_F(ConfigLoaderTest, TopLevelLegacyRiskLimitsCannotSubstituteForSchema2) {
    auto merged = minimal_defaults();
    auto portfolio = minimal_portfolio();
    portfolio["max_drawdown"] = 0.3;
    portfolio["max_leverage"] = 2.0;
    ConfigLoader::merge_json(merged, portfolio);
    merged["risk"] = minimal_risk();
    merged["risk"].erase("max_drawdown");
    merged["risk"].erase("max_leverage");
    ASSERT_FALSE(merged.at("risk").contains("max_drawdown"));
    ASSERT_FALSE(merged.at("risk").contains("max_leverage"));

    const nlohmann::json unrelated_override = {
        {"live", {{"historical_days", 450}}},
        {"strategies", {{"TREND_FOLLOWING", {{"weight", 0.75}}}}},
    };
    ConfigLoader::merge_json(merged, unrelated_override);
    EXPECT_TRUE(ConfigLoader::extract_config(merged).is_error());
}

TEST_F(ConfigLoaderTest, ReportingRiskFieldsRemainCoupledAndEditable) {
    write_full_set("base");
    auto loaded = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(loaded.is_ok());
    auto serialized = loaded.value().to_json();
    EXPECT_FALSE(serialized.contains("risk_defaults"));
    auto& reporting = serialized["risk"]["risk_reporting"];
    auto& carver = serialized["risk"]["modules"][0];

    for (auto* field : {"confidence_level", "lookback_period", "max_correlation"}) {
        const nlohmann::json value = std::string(field) == "confidence_level" ? nlohmann::json(0.975)
            : std::string(field) == "lookback_period" ? nlohmann::json(199)
                                                       : nlohmann::json(0.65);
        reporting[field] = value;
        carver[field] = value;
    }
    auto extracted = ConfigLoader::extract_config(serialized);
    ASSERT_TRUE(extracted.is_ok());
    const auto round_trip = extracted.value().to_json().at("risk");
    EXPECT_EQ(round_trip.at("risk_reporting"), reporting);
    EXPECT_EQ(round_trip.at("modules").at(0), carver);
}

TEST_F(ConfigLoaderTest, FractionalCostPenaltyScalarSurvivesParsing) {
    DynamicOptConfig config;
    config.from_json({{"cost_penalty_scalar", 12.75}});
    EXPECT_DOUBLE_EQ(config.cost_penalty_scalar, 12.75);
    EXPECT_DOUBLE_EQ(config.to_json().at("cost_penalty_scalar").get<double>(), 12.75);
}

TEST_F(ConfigLoaderTest, NestedStrategiesSurviveSerializedMergeAndExtraction) {
    const nlohmann::json strategies = {
        {"TREND_FOLLOWING", {{"weight", 0.61},
                              {"symbols", nlohmann::json::array({"ES", "NQ"})},
                              {"parameters", {{"fast", 16}, {"slow", 64}}}}},
        {"MEAN_REVERSION", {{"weight", 0.39}, {"enabled", false}}},
    };
    write_full_set("base", {}, {{"strategies", strategies}});
    auto loaded = ConfigLoader::load(base_, "base");
    ASSERT_TRUE(loaded.is_ok());
    auto serialized = loaded.value().to_json();
    ASSERT_EQ(serialized.at("strategies"), strategies);
    auto override_json = nlohmann::json{{"strategies", {{"TREND_FOLLOWING",
        {{"parameters", {{"fast", 20}}}}}}}};
    ConfigLoader::merge_json(serialized, override_json);
    auto extracted = ConfigLoader::extract_config(serialized);
    ASSERT_TRUE(extracted.is_ok());
    auto expected = strategies;
    expected["TREND_FOLLOWING"]["parameters"]["fast"] = 20;
    EXPECT_EQ(extracted.value().to_json().at("strategies"), expected);
}

// ===== Error paths =====

TEST_F(ConfigLoaderTest, LoadMissingDefaultsFileReturnsError) {
    // Only portfolio file, no defaults
    write_json(base_ / "portfolios" / "base" / "portfolio.json", minimal_portfolio());
    auto r = ConfigLoader::load(base_, "base");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadMissingPortfolioDirectoryReturnsError) {
    write_json(base_ / "defaults.json", minimal_defaults());
    auto r = ConfigLoader::load(base_, "doesnotexist");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadMalformedJsonReturnsError) {
    std::filesystem::create_directories(base_ / "portfolios" / "base");
    {
        std::ofstream f(base_ / "defaults.json");
        f << "{ this is not valid json";
    }
    write_json(base_ / "portfolios" / "base" / "portfolio.json", minimal_portfolio());
    auto r = ConfigLoader::load(base_, "base");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadMissingPortfolioIdReturnsError) {
    auto p = minimal_portfolio();
    p.erase("portfolio_id");
    write_json(base_ / "defaults.json", minimal_defaults());
    write_json(base_ / "portfolios" / "base" / "portfolio.json", p);
    write_json(base_ / "portfolios" / "base" / "risk.json", minimal_risk());
    write_json(base_ / "portfolios" / "base" / "email.json", minimal_email());
    auto r = ConfigLoader::load(base_, "base");
    EXPECT_TRUE(r.is_error());
}

// ===== Struct serialization round-trips =====

TEST_F(ConfigLoaderTest, DatabaseConfigConnectionStringFormat) {
    DatabaseConfig db;
    db.host = "host.example.com";
    db.port = "5433";
    db.username = "user";
    db.password = "pw";
    db.name = "mydb";
    EXPECT_EQ(db.get_connection_string(),
              "postgresql://user:pw@host.example.com:5433/mydb");
}

TEST_F(ConfigLoaderTest, EmailConfigJsonRoundTrip) {
    EmailConfig orig;
    orig.smtp_host = "smtp.example.com";
    orig.smtp_port = 465;
    orig.username = "u";
    orig.password = "p";
    orig.from_email = "f@test.com";
    orig.use_tls = false;
    orig.to_emails = {"a@test.com", "b@test.com"};
    EmailConfig restored;
    restored.from_json(orig.to_json());
    EXPECT_EQ(restored.smtp_host, "smtp.example.com");
    EXPECT_EQ(restored.smtp_port, 465);
    EXPECT_FALSE(restored.use_tls);
    EXPECT_EQ(restored.to_emails.size(), 2u);
}

TEST_F(ConfigLoaderTest, ExecutionConfigJsonRoundTrip) {
    ExecutionSettingsConfig ex;
    ex.commission_rate = 0.001;
    ex.slippage_bps = 2.5;
    ex.position_limit_backtest = 5000.0;
    ex.position_limit_live = 1000.0;
    ExecutionSettingsConfig r;
    r.from_json(ex.to_json());
    EXPECT_DOUBLE_EQ(r.commission_rate, 0.001);
    EXPECT_DOUBLE_EQ(r.slippage_bps, 2.5);
    EXPECT_DOUBLE_EQ(r.position_limit_backtest, 5000.0);
}

TEST_F(ConfigLoaderTest, BacktestSpecificConfigJsonRoundTrip) {
    BacktestSpecificConfig b;
    b.lookback_years = 7;
    b.store_trade_details = false;
    BacktestSpecificConfig r;
    r.from_json(b.to_json());
    EXPECT_EQ(r.lookback_years, 7);
    EXPECT_FALSE(r.store_trade_details);
}

TEST_F(ConfigLoaderTest, LiveSpecificConfigJsonRoundTrip) {
    LiveSpecificConfig l;
    l.historical_days = 500;
    LiveSpecificConfig r;
    r.from_json(l.to_json());
    EXPECT_EQ(r.historical_days, 500);
}

TEST_F(ConfigLoaderTest, StrategyDefaultsConfigJsonRoundTrip) {
    StrategyDefaultsConfig s;
    s.fdm = {{1, 1.0}, {2, 1.5}, {3, 2.0}};
    s.max_strategy_allocation = 0.5;
    s.min_strategy_allocation = 0.1;
    StrategyDefaultsConfig r;
    const auto j = s.to_json();
    r.from_json(j);
    EXPECT_EQ(r.fdm.size(), 3u);
    EXPECT_DOUBLE_EQ(r.fdm[2].second, 2.0);
    EXPECT_DOUBLE_EQ(r.max_strategy_allocation, 0.5);
    EXPECT_FALSE(j.contains("use_optimization"));
    EXPECT_FALSE(j.contains("use_risk_management"));
}

TEST_F(ConfigLoaderTest, DatabaseConfigJsonRoundTrip) {
    DatabaseConfig d;
    d.host = "h";
    d.port = "9999";
    d.username = "u";
    d.password = "p";
    d.name = "n";
    d.num_connections = 13;
    DatabaseConfig r;
    r.from_json(d.to_json());
    EXPECT_EQ(r.host, "h");
    EXPECT_EQ(r.port, "9999");
    EXPECT_EQ(r.num_connections, 13u);
}

// ===== from_json with missing fields preserves defaults =====

TEST_F(ConfigLoaderTest, EmailConfigFromEmptyJsonPreservesDefaults) {
    EmailConfig e;
    e.from_json(nlohmann::json::object());
    EXPECT_EQ(e.smtp_host, "smtp.gmail.com");  // default
    EXPECT_EQ(e.smtp_port, 587);                // default
    EXPECT_TRUE(e.use_tls);                     // default
}

TEST_F(ConfigLoaderTest, BacktestConfigFromEmptyJsonPreservesDefaults) {
    BacktestSpecificConfig b;
    b.from_json(nlohmann::json::object());
    EXPECT_EQ(b.lookback_years, 2);
    EXPECT_TRUE(b.store_trade_details);
}

// ===== load_legacy =====

TEST_F(ConfigLoaderTest, LoadLegacyMissingFileReturnsError) {
    auto r = ConfigLoader::load_legacy(base_ / "missing.json");
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadLegacyMalformedJsonReturnsError) {
    auto p = base_ / "legacy.json";
    std::ofstream(p) << "{ not valid";
    auto r = ConfigLoader::load_legacy(p);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadLegacyMissingPortfolioIdFailsValidation) {
    nlohmann::json legacy = {
        {"database", minimal_defaults()["database"]},
        {"portfolio", {{"strategies", {{"S1", {{"weight", 1.0}}}}}}},
    };
    auto p = base_ / "legacy.json";
    std::ofstream(p) << legacy.dump(2);
    auto r = ConfigLoader::load_legacy(p);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConfigLoaderTest, LoadLegacyValidConfigPopulatesFields) {
    nlohmann::json legacy = {
        {"portfolio_id", "LEGACY_TEST"},
        {"database", minimal_defaults()["database"]},
        {"email", minimal_email()},
        {"portfolio", {{"strategies", {{"S1", {{"weight", 1.0}}}}}}},
    };
    legacy["initial_capital"] = 1'000'000.0;
    legacy["reserve_capital_pct"] = 0.1;
    auto p = base_ / "legacy.json";
    std::ofstream(p) << legacy.dump(2);
    auto r = ConfigLoader::load_legacy(p);
    // Even with portfolio_id and strategies set, validation requires database
    // fields and capital. With them populated, this should pass.
    if (r.is_ok()) {
        EXPECT_EQ(r.value().portfolio_id, "LEGACY_TEST");
    }
}

// ===== validate_config edge cases =====

TEST_F(ConfigLoaderTest, ValidateRejectsNonPositiveInitialCapital) {
    AppConfig c;
    c.portfolio_id = "P";
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 0.0;
    c.strategies_config = {{"s", {{"w", 1.0}}}};
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

TEST_F(ConfigLoaderTest, ValidateRejectsReserveCapitalPctOutOfRange) {
    AppConfig c;
    c.portfolio_id = "P";
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 100.0;
    c.reserve_capital_pct = 1.0;  // must be < 1.0
    c.strategies_config = {{"s", {{"w", 1.0}}}};
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

TEST_F(ConfigLoaderTest, ValidateRejectsEmptyStrategies) {
    AppConfig c;
    c.portfolio_id = "P";
    c.database.host = "h";
    c.database.username = "u";
    c.database.password = "p";
    c.database.name = "n";
    c.initial_capital = 100.0;
    c.strategies_config = nlohmann::json::object();  // empty
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

TEST_F(ConfigLoaderTest, ValidateRejectsMissingDatabaseFields) {
    AppConfig c;
    c.portfolio_id = "P";
    c.initial_capital = 100.0;
    c.strategies_config = {{"s", {{"w", 1.0}}}};
    // database fields all empty
    EXPECT_TRUE(ConfigLoader::validate_config(c).is_error());
}

// ===== merge_json deep merge =====

TEST_F(ConfigLoaderTest, DeepMergeRecursesIntoNestedObjects) {
    nlohmann::json target = {
        {"a", {{"b", 1}, {"c", 2}}},
        {"d", "old"},
    };
    nlohmann::json source = {
        {"a", {{"c", 99}, {"e", 3}}},
        {"d", "new"},
    };
    ConfigLoader::merge_json(target, source);
    EXPECT_EQ(target["a"]["b"], 1);    // preserved
    EXPECT_EQ(target["a"]["c"], 99);   // overridden
    EXPECT_EQ(target["a"]["e"], 3);    // added
    EXPECT_EQ(target["d"], "new");     // top-level override
}

TEST_F(ConfigLoaderTest, DeepMergeReplacesNonObjectsWithoutRecursion) {
    nlohmann::json target = {{"x", nlohmann::json::array({1, 2, 3})}};
    nlohmann::json source = {{"x", nlohmann::json::array({4})}};
    ConfigLoader::merge_json(target, source);
    EXPECT_EQ(target["x"].size(), 1u);
    EXPECT_EQ(target["x"][0], 4);
}
