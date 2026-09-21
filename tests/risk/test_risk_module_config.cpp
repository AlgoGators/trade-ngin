// parse_risk_schema: one test per rule, asserting the EXACT message.
//
// The message is the whole value of a fail-closed rule. A loader that refuses a config
// and says "invalid risk configuration" has moved the operator's problem from "the book
// gated on a value nobody wrote" to "the book will not start and I cannot see why", which
// is not obviously an improvement. So every rule is pinned by its words, and a rule whose
// words change has to be changed here on purpose.
//
// The rules, by the names the design gives them: T0 (a schema-1 file), S1-S6 (structure),
// S8 (term ownership), S9 (REFUSE on a multi-sleeve book), R1-R9 (ranges), A1/A2 (parsed
// but not yet implemented), C1 (the reporter mirrors the gate).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"
#include "risk_module_test_helpers.hpp"

using namespace trade_ngin;

namespace {

const char* kP = "TEST_PORTFOLIO";

nlohmann::json carver(const char* id = "carver") {
    return {
        {"id", id},
        {"type", "carver"},
        {"var_limit", 0.25},
        {"jump_risk_limit", 0.05},
        {"max_correlation", 0.85},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
        {"confidence_level", 0.99},
        {"lookback_period", 252},
        {"lookback_unit", "dates"},
        {"min_gate_dates", 21},
        {"missing_symbol_policy", "ignore"},
        {"_missing_symbol_policy_reason", "migrated literally"},
    };
}

nlohmann::json reporting() {
    return {
        {"type", "carver"},
        {"window", "all_bars"},
        {"var_limit", 0.25},
        {"jump_risk_limit", 0.05},
        {"max_correlation", 0.85},
        {"max_gross_leverage", 4.0},
        {"max_net_leverage", 2.0},
        {"confidence_level", 0.99},
        {"lookback_period", 252},
    };
}

nlohmann::json none_module(const char* id = "none") {
    return {{"id", id}, {"type", "none"}, {"_reason", "measured alone"},
            {"_ruled_by", "HD"}, {"_ruled_on", "2026-09-18"}};
}

nlohmann::json warn_module(const char* id = "w") {
    return {{"id", id}, {"type", "warn"},
            {"condition", {{"kind", "lap_at_least"}, {"threshold", 3}}},
            {"reason", "still cutting on the third lap"}};
}

nlohmann::json refuse_module(const char* id = "r") {
    return {{"id", id}, {"type", "refuse"},
            {"condition", {{"kind", "max_abs_quantity_above"}, {"threshold", 5000}}},
            {"reason", "a line that size is a sizing error"}};
}

nlohmann::json constant_scale_module(const char* id = "cs") {
    return {{"id", id}, {"type", "constant_scale"}, {"scale", 0.5}};
}

// A vector, not a nlohmann::json: `risk_with({carver()})` with a json parameter would
// select json's COPY constructor for the single-element brace list and hand the parser an
// object where it wants an array.
nlohmann::json risk_with(const std::vector<nlohmann::json>& modules) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& module : modules) array.push_back(module);
    return {{"schema", 2},
            {"modules", std::move(array)},
            {"risk_reporting", reporting()},
            {"max_drawdown", 0.3},
            {"max_leverage", 2.0}};
}

/// R10 (T-6b commit 7d): a book no module of which is a `carver` cannot cut itself, so it
/// carries the signature of whoever decided that. Several cases below are deliberately
/// carver-less in order to test a different rule, and stamp it here rather than restate it.
nlohmann::json ruled(nlohmann::json risk) {
    risk["_ruled_by"] = "unit test";
    risk["_ruled_on"] = "2026-09-20";
    return risk;
}

nlohmann::json one_sleeve() {
    return {{"TREND_FOLLOWING", {{"enabled_backtest", true}, {"type", "TrendFollowingStrategy"}}}};
}

nlohmann::json two_sleeves() {
    return {{"TREND_FOLLOWING", {{"enabled_live", true}, {"type", "TrendFollowingStrategy"}}},
            {"TREND_FOLLOWING_FAST",
             {{"enabled_live", true}, {"type", "TrendFollowingFastStrategy"}}}};
}

/// The error text, or "" when the config parsed.
std::string parse_error(const nlohmann::json& risk,
                        const nlohmann::json& sleeves = nlohmann::json(),
                        const nlohmann::json& strategies = one_sleeve()) {
    auto r = parse_risk_schema(risk, sleeves, strategies, kP);
    if (r.is_ok()) return "";
    return r.error()->what();
}

const char* kPrefix = "risk config for TEST_PORTFOLIO: ";
std::string msg(const std::string& tail) {
    return kPrefix + tail;
}

}  // namespace

// ===== The shipped shape parses =====

TEST(RiskSchemaParse, TheShippedShapeParsesIntoTheValuesTheFileWrites) {
    auto r = parse_risk_schema(risk_with({carver()}), nlohmann::json(), one_sleeve(), kP);
    ASSERT_TRUE(r.is_ok()) << (r.is_error() ? r.error()->what() : "");
    const RiskSchema& s = r.value();
    EXPECT_EQ(s.schema, 2);
    ASSERT_EQ(s.portfolio.size(), 1u);
    EXPECT_EQ(s.portfolio[0].id, "carver");
    EXPECT_EQ(s.portfolio[0].type, "carver");
    const auto* c = std::get_if<CarverModuleConfig>(&s.portfolio[0].params);
    ASSERT_NE(c, nullptr);
    EXPECT_DOUBLE_EQ(c->var_limit, 0.25);
    EXPECT_DOUBLE_EQ(c->max_correlation, 0.85);
    EXPECT_EQ(c->lookback_period, 252);
    EXPECT_EQ(c->min_gate_dates, 21);
    EXPECT_EQ(c->missing_symbol_policy, "ignore");
    EXPECT_EQ(c->missing_symbol_policy_reason, "migrated literally");
    EXPECT_DOUBLE_EQ(s.max_drawdown, 0.3);
    EXPECT_DOUBLE_EQ(s.max_leverage, 2.0);
    // The reporter is what AppConfig::risk_config is back-filled from.
    const RiskConfig back_fill = s.reporting.to_risk_config();
    EXPECT_DOUBLE_EQ(back_fill.var_limit, 0.25);
    EXPECT_DOUBLE_EQ(back_fill.max_gross_leverage, 4.0);
    EXPECT_EQ(back_fill.lookback_period, 252);
    // to_risk_config leaves capital and version at the struct's values for the caller,
    // exactly as every runner sets them today.
    EXPECT_EQ(back_fill.version, "1.0.0");
    EXPECT_FALSE(s.is_none());
}

TEST(RiskSchemaParse, ALoneNoneIsTheOnlyWayToSayTheBookRunsNoRiskLayer) {
    auto r = parse_risk_schema(risk_with({none_module()}), nlohmann::json(), one_sleeve(), kP);
    ASSERT_TRUE(r.is_ok()) << (r.is_error() ? r.error()->what() : "");
    EXPECT_TRUE(r.value().is_none());
    const auto* n = std::get_if<NoneModuleConfig>(&r.value().portfolio[0].params);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->reason, "measured alone");
    EXPECT_EQ(n->ruled_by, "HD");
    EXPECT_EQ(n->ruled_on, "2026-09-18");
    EXPECT_TRUE(r.value().portfolio[0].terms().empty());
}

// ===== T0: a schema-1 file is named as such =====

TEST(RiskSchemaParse, T0NamesASchema1FileAndGivesTheMigrationCommand) {
    nlohmann::json schema1 = {{"var_limit", 0.15}, {"max_drawdown", 0.4}, {"max_leverage", 4.0}};
    const std::string expected = msg(
        "risk.json is schema 1 (flat gating keys, no \"schema\"/\"modules\"); migrate it with: "
        "python3 scripts/migrate_risk_json.py <config dir> --in-place");
    EXPECT_EQ(parse_error(schema1), expected);
    EXPECT_EQ(std::string(check_not_schema1(schema1, kP).error()->what()), expected);
    // A file with a "schema" key is not schema 1, whatever else is wrong with it.
    EXPECT_TRUE(check_not_schema1(risk_with({carver()}), kP).is_ok());
}

// ===== S1, S2: the top level =====

TEST(RiskSchemaParse, S1SchemaAndModulesAreRequired) {
    nlohmann::json no_schema = {{"modules", nlohmann::json::array({carver()})}};
    EXPECT_EQ(parse_error(no_schema), msg("risk.schema is required and must be 2"));
    nlohmann::json schema1_number = risk_with({carver()});
    schema1_number["schema"] = 1;
    EXPECT_EQ(parse_error(schema1_number), msg("risk.schema is required and must be 2"));

    nlohmann::json no_modules = {{"schema", 2}, {"risk_reporting", reporting()},
                                 {"max_drawdown", 0.3}, {"max_leverage", 2.0}};
    EXPECT_EQ(parse_error(no_modules),
              msg("risk.modules is required; name the module(s) this portfolio runs, or a single "
                  "{\"type\": \"none\"} carrying _reason, _ruled_by and _ruled_on"));
}

TEST(RiskSchemaParse, S2AnEmptyModuleListIsAnErrorNotAnAssignment) {
    EXPECT_EQ(parse_error(risk_with({})),
              msg("risk.modules is empty; an omission and a decision must not share an encoding "
                  "(use {\"type\": \"none\"} with _reason, _ruled_by, _ruled_on)"));
    nlohmann::json not_array = risk_with({carver()});
    not_array["modules"] = "carver";
    EXPECT_EQ(parse_error(not_array), msg("risk.modules must be an array of module objects"));

    // The same at sleeve scope.
    nlohmann::json sleeves = {{"TREND_FOLLOWING", nlohmann::json::array()}};
    EXPECT_EQ(parse_error(risk_with({carver()}), sleeves),
              msg("sleeve_risk_modules.TREND_FOLLOWING is empty; an omission and a decision must "
                  "not share an encoding (use {\"type\": \"none\"} with _reason, _ruled_by, "
                  "_ruled_on)"));
}

TEST(RiskSchemaParse, S5RejectsAnUnknownTopLevelKey) {
    nlohmann::json risk = risk_with({carver()});
    risk["max_correlation"] = 0.9;  // schema 1's home for it, left behind by a hand edit
    EXPECT_EQ(parse_error(risk),
              msg("risk.max_correlation is not a top-level key of a schema-2 risk.json"));
    // A `_` key is a comment at every level.
    nlohmann::json commented = risk_with({carver()});
    commented["_description"] = "a note";
    EXPECT_EQ(parse_error(commented), "");
}

// ===== S3: `none` carries its ruling =====

TEST(RiskSchemaParse, S3NoneNeedsEachOfItsThreeAttributionKeys) {
    for (const char* key : {"_reason", "_ruled_by", "_ruled_on"}) {
        nlohmann::json m = none_module();
        m.erase(key);
        EXPECT_EQ(parse_error(risk_with({m})),
                  msg(std::string("risk.modules[0].") + key +
                      " is required for a \"none\" module (schema 2 has no defaults)"))
            << key;
        nlohmann::json empty = none_module();
        empty[key] = "";
        EXPECT_EQ(parse_error(risk_with({empty})),
                  msg(std::string("risk.modules[0] is type \"none\" and needs a non-empty ") +
                      key))
            << key;
    }
}

TEST(RiskSchemaParse, S3RuledOnMustBeADate) {
    nlohmann::json m = none_module();
    m["_ruled_on"] = "18/09/2026";
    EXPECT_EQ(parse_error(risk_with({m})),
              msg("risk.modules[0]._ruled_on must be YYYY-MM-DD, got \"18/09/2026\""));
}

TEST(RiskSchemaParse, S3NoneIsTheWholeAnswerForItsScopeOrNoAnswerAtAll) {
    EXPECT_EQ(parse_error(risk_with({none_module(), carver()})),
              msg("risk.modules[0] is type \"none\" and must be the only module in its scope"));
}

TEST(RiskSchemaParse, S3NoneIsNotValidAtSleeveScope) {
    nlohmann::json sleeves = {{"TREND_FOLLOWING", nlohmann::json::array({none_module("sn")})}};
    EXPECT_EQ(parse_error(risk_with({carver()}), sleeves),
              msg("sleeve_risk_modules.TREND_FOLLOWING[0] is type \"none\", which is only valid "
                  "at portfolio scope; omit the sleeve's entry instead"));
}

// ===== S4, S5, S6: the module object =====

TEST(RiskSchemaParse, S4RejectsAnUnknownOrMissingType) {
    nlohmann::json m = carver();
    m["type"] = "carvr";
    EXPECT_EQ(parse_error(risk_with({m})),
              msg("risk.modules[0].type \"carvr\" is not a known risk module type (carver, none, "
                  "constant_scale, warn, refuse)"));
    nlohmann::json no_type = carver();
    no_type.erase("type");
    EXPECT_EQ(parse_error(risk_with({no_type})), msg("risk.modules[0].type is required"));
}

TEST(RiskSchemaParse, S5RejectsAnUnknownKeyAndAMissingRequiredOne) {
    nlohmann::json typo = carver();
    typo.erase("var_limit");
    typo["var_limt"] = 0.25;
    EXPECT_EQ(parse_error(risk_with({typo})),
              msg("risk.modules[0].var_limt is not a key of a \"carver\" module"));

    nlohmann::json missing = carver();
    missing.erase("min_gate_dates");
    EXPECT_EQ(parse_error(risk_with({missing})),
              msg("risk.modules[0].min_gate_dates is required for a \"carver\" module (schema 2 "
                  "has no defaults)"));
}

TEST(RiskSchemaParse, S5ModuleIdsAreUniqueAcrossTheWholeBook) {
    EXPECT_EQ(parse_error(risk_with({constant_scale_module("dup"), warn_module("dup")})),
              msg("module id \"dup\" is used twice (risk.modules[0], risk.modules[1])"));

    nlohmann::json sleeves = {
        {"TREND_FOLLOWING", nlohmann::json::array({constant_scale_module("carver")})}};
    EXPECT_EQ(parse_error(risk_with({carver()}), sleeves),
              msg("module id \"carver\" is used twice (risk.modules[0], "
                  "sleeve_risk_modules.TREND_FOLLOWING[0])"));
}

TEST(RiskSchemaParse, S5AppliesToTheReporterToo) {
    nlohmann::json risk = risk_with({carver()});
    risk["risk_reporting"]["lookback_unit"] = "bars";
    EXPECT_EQ(parse_error(risk),
              msg("risk.risk_reporting.lookback_unit is not a key of a \"carver\" risk reporter"));
}

TEST(RiskSchemaParse, S6EnabledIsNotAKey) {
    for (bool value : {true, false}) {
        nlohmann::json m = carver();
        m["enabled"] = value;
        EXPECT_EQ(parse_error(risk_with({m})),
                  msg("risk.modules[0].enabled was removed in schema 2 (HD Q8): a module the book "
                      "does not run is a module the book does not list"))
            << value;
    }
}

// ===== S8: one composition term per chain =====

TEST(RiskSchemaParse, S8TwoCarverModulesAtPortfolioScopeIsAnError) {
    EXPECT_EQ(parse_error(risk_with({carver("a"), carver("b")})),
              msg("composition terms (correlation/VaR/jump) are owned by risk.modules[0] and "
                  "again by risk.modules[1]; a composition term may be active once along a "
                  "sleeve->portfolio chain"));
}

TEST(RiskSchemaParse, S8ACarverAtBothScopesIsAnError) {
    nlohmann::json sleeves = {{"TREND_FOLLOWING", nlohmann::json::array({carver("sleeve_carver")})}};
    // T-6b commit 7d: a carver at SLEEVE scope is now refused while the module is still being
    // parsed (C-2: it divides by the portfolio's capital), so S8's chain rule never sees this
    // pair any more. The stricter message wins, and this asserts that ordering rather than
    // pretending the old one still fires. S8 itself is still exercised by
    // S8TwoCarverModulesAtPortfolioScopeIsAnError and by the constant_scale pair below.
    EXPECT_EQ(parse_error(risk_with({carver()}), sleeves),
              msg("sleeve_risk_modules.TREND_FOLLOWING[0] is type \"carver\", which is only "
                  "valid at portfolio scope: the Carver gate divides by the portfolio's capital, "
                  "so at sleeve scope its leverage limits would be read against the whole book's "
                  "money"));
    // Magnitude terms compose, so a constant scale at both scopes is fine.
    nlohmann::json ok_sleeves = {
        {"TREND_FOLLOWING", nlohmann::json::array({constant_scale_module("sleeve_cut")})}};
    EXPECT_EQ(parse_error(ruled(risk_with({constant_scale_module()})), ok_sleeves), "");
}

// ===== S9: no REFUSE on a multi-sleeve book before T-BASE =====

TEST(RiskSchemaParse, S9ARefuseCapableModuleNeedsASingleSleeveBook) {
    EXPECT_EQ(parse_error(risk_with({refuse_module()}), nlohmann::json(), two_sleeves()),
              msg("risk.modules[0] can REFUSE, and a book with 2 sleeves cannot pin every sleeve "
                  "to a stored T-1 until T-BASE (T-RISK-ARCH_ADVERSARIAL B1)"));
    // One sleeve: accepted. (Carver-less, so R10's attribution goes with it.)
    EXPECT_EQ(parse_error(ruled(risk_with({refuse_module()})), nlohmann::json(), one_sleeve()),
              "");
    // A disabled strategy is not a sleeve (the predicate config_loader.cpp's G-03 uses).
    nlohmann::json one_enabled = two_sleeves();
    one_enabled["TREND_FOLLOWING_FAST"]["enabled_live"] = false;
    EXPECT_EQ(parse_error(ruled(risk_with({refuse_module()})), nlohmann::json(), one_enabled),
              "");
}

// ===== R1-R8: the ranges =====

TEST(RiskSchemaParse, R1MaxCorrelationMustBeStrictlyInsideZeroAndOne) {
    const std::string tail =
        " must be in (0, 1), got %; at 1.0 the correlation term can never bind "
        "(risk_manager.cpp:551 clamps rho)";
    for (const char* value : {"1.0", "0.0"}) {
        nlohmann::json m = carver();
        m["max_correlation"] = nlohmann::json::parse(value);
        std::string want = msg("risk.modules[0].max_correlation" + tail);
        want.replace(want.find('%'), 1, value);
        EXPECT_EQ(parse_error(risk_with({m})), want) << value;
    }
}

TEST(RiskSchemaParse, R2AndR3VarAndJumpLimitsAreHalfOpenOnZeroAndClosedOnOne) {
    nlohmann::json zero = carver();
    zero["var_limit"] = 0.0;
    EXPECT_EQ(parse_error(risk_with({zero})),
              msg("risk.modules[0].var_limit must be in (0, 1], got 0.0"));
    nlohmann::json over = carver();
    over["jump_risk_limit"] = 1.5;
    EXPECT_EQ(parse_error(risk_with({over})),
              msg("risk.modules[0].jump_risk_limit must be in (0, 1], got 1.5"));
    // 1.0 is the inclusive bound on both.
    nlohmann::json edge = carver();
    edge["var_limit"] = 1.0;
    edge["jump_risk_limit"] = 1.0;
    nlohmann::json risk = risk_with({edge});
    risk["risk_reporting"]["var_limit"] = 1.0;
    risk["risk_reporting"]["jump_risk_limit"] = 1.0;
    EXPECT_EQ(parse_error(risk), "");
}

TEST(RiskSchemaParse, R4LeverageLimitsAreOrderedAndCapped) {
    const std::string head = "risk.modules[0].leverage limits must satisfy 0 < max_net_leverage "
                             "<= max_gross_leverage <= 10, got net ";
    nlohmann::json inverted = carver();
    inverted["max_net_leverage"] = 5.0;
    inverted["max_gross_leverage"] = 4.0;
    EXPECT_EQ(parse_error(risk_with({inverted})), msg(head + "5.0 gross 4.0"));

    nlohmann::json over_ceiling = carver();
    over_ceiling["max_gross_leverage"] = 10.5;
    over_ceiling["max_net_leverage"] = 10.5;
    EXPECT_EQ(parse_error(risk_with({over_ceiling})), msg(head + "10.5 gross 10.5"));

    nlohmann::json zero_net = carver();
    zero_net["max_net_leverage"] = 0.0;
    EXPECT_EQ(parse_error(risk_with({zero_net})), msg(head + "0.0 gross 4.0"));
}

TEST(RiskSchemaParse, R5AndR6ConfidenceAndLookback) {
    nlohmann::json conf = carver();
    conf["confidence_level"] = 1.0;
    EXPECT_EQ(parse_error(risk_with({conf})),
              msg("risk.modules[0].confidence_level must be in (0, 1), got 1.0"));
    nlohmann::json look = carver();
    look["lookback_period"] = 0;
    EXPECT_EQ(parse_error(risk_with({look})),
              msg("risk.modules[0].lookback_period must be a positive integer, got 0"));
    nlohmann::json fractional = carver();
    fractional["lookback_period"] = 252.5;
    EXPECT_EQ(parse_error(risk_with({fractional})),
              msg("risk.modules[0].lookback_period must be a positive integer, got 252.5"));
}

TEST(RiskSchemaParse, R7TheWindowUnitAndTheGateFloorMustAgreeInTheSameUnit) {
    nlohmann::json weeks = carver();
    weeks["lookback_unit"] = "weeks";
    EXPECT_EQ(parse_error(risk_with({weeks})),
              msg("risk.modules[0].lookback_unit must be \"dates\", got \"weeks\""));

    nlohmann::json tiny = carver();
    tiny["min_gate_dates"] = 2;
    EXPECT_EQ(parse_error(risk_with({tiny})),
              msg("risk.modules[0].min_gate_dates must be an integer >= 3 (two dates give a NaN "
                  "covariance), got 2"));

    // Both sides are counted in dates: the window keeps lookback_period distinct dates.
    nlohmann::json short_window = carver();
    short_window["lookback_period"] = 20;
    nlohmann::json risk = risk_with({short_window});
    risk["risk_reporting"]["lookback_period"] = 20;
    EXPECT_EQ(parse_error(risk),
              msg("risk.modules[0].lookback_period (20 dates) is shorter than min_gate_dates "
                  "(21 dates)"));
}

TEST(RiskSchemaParse, R8MissingSymbolPolicyIsRequiredAndIgnoreMustSayWhy) {
    nlohmann::json no_policy = carver();
    no_policy.erase("missing_symbol_policy");
    EXPECT_EQ(parse_error(risk_with({no_policy})),
              msg("risk.modules[0].missing_symbol_policy is required for a \"carver\" module "
                  "(schema 2 has no defaults)"));

    nlohmann::json bad = carver();
    bad["missing_symbol_policy"] = "skip";
    EXPECT_EQ(parse_error(risk_with({bad})),
              msg("risk.modules[0].missing_symbol_policy must be \"ignore\", \"warn\" or "
                  "\"refuse\", got \"skip\""));

    nlohmann::json unexplained = carver();
    unexplained.erase("_missing_symbol_policy_reason");
    EXPECT_EQ(parse_error(risk_with({unexplained})),
              msg("risk.modules[0].missing_symbol_policy \"ignore\" is the fail-open behaviour "
                  "and needs a non-empty _missing_symbol_policy_reason"));
}

TEST(RiskSchemaParse, R9TheTwoStrategyLimitsAreRequiredAndRanged) {
    nlohmann::json risk = risk_with({carver()});
    risk.erase("max_drawdown");
    EXPECT_EQ(parse_error(risk), msg("risk.max_drawdown is required and must be in (0, 1]"));
    nlohmann::json risk2 = risk_with({carver()});
    risk2["max_leverage"] = 0.0;
    EXPECT_EQ(parse_error(risk2),
              msg("risk.max_leverage is required and must be > 0 (it sizes the book: "
                  "trend_following.cpp:1216)"));
}

TEST(RiskSchemaParse, RcsTheTestModulesOwnParameters) {
    nlohmann::json cs = constant_scale_module();
    cs["scale"] = 1.5;
    EXPECT_EQ(parse_error(risk_with({cs})),
              msg("risk.modules[0].scale must be in (0, 1], got 1.5"));
    nlohmann::json cs0 = constant_scale_module();
    cs0["scale"] = 0.0;
    EXPECT_EQ(parse_error(risk_with({cs0})),
              msg("risk.modules[0].scale must be in (0, 1], got 0.0"));

    nlohmann::json bad_kind = warn_module();
    bad_kind["condition"]["kind"] = "leverage_above";
    EXPECT_EQ(parse_error(risk_with({bad_kind})),
              msg("risk.modules[0].condition.kind must be \"always\", \"never\", "
                  "\"lap_at_least\", \"nonzero_positions_above\" or \"max_abs_quantity_above\", "
                  "got \"leverage_above\""));

    nlohmann::json pointless = warn_module();
    pointless["condition"] = {{"kind", "always"}, {"threshold", 3}};
    EXPECT_EQ(parse_error(risk_with({pointless})),
              msg("risk.modules[0].condition.threshold is meaningless for a \"always\" condition; "
                  "remove it"));

    nlohmann::json no_threshold = warn_module();
    no_threshold["condition"] = {{"kind", "lap_at_least"}};
    EXPECT_EQ(parse_error(risk_with({no_threshold})),
              msg("risk.modules[0].condition.threshold is required for a \"lap_at_least\" "
                  "condition"));

    nlohmann::json silent = refuse_module();
    silent["reason"] = "";
    EXPECT_EQ(parse_error(risk_with({silent})),
              msg("risk.modules[0].reason must be a non-empty string (it is what the log says "
                  "when the module fires), got \"\""));
}

// ===== A1, A2: parsed, validated, and not yet implemented =====

TEST(A1AndA2, TheUnimplementedOptionsAreRefusedByNameWithTheCommitThatLandsThem) {
    // A1 landed with T-6b commit 9: "dates" is the unit and loads (carver() carries it).
    EXPECT_EQ(parse_error(risk_with({carver()})), "");

    for (const char* policy : {"warn", "refuse"}) {
        nlohmann::json m = carver();
        m["missing_symbol_policy"] = policy;
        m.erase("_missing_symbol_policy_reason");
        EXPECT_EQ(parse_error(risk_with({m})),
                  msg(std::string("risk.modules[0].missing_symbol_policy \"") + policy +
                      "\" is not implemented yet; only \"ignore\" (with its reason) is accepted"))
            << policy;
    }
}

// ===== C1: the reporter mirrors the gate =====

TEST(RiskSchemaParse, C1TheReporterIsRequired) {
    nlohmann::json risk = risk_with({carver()});
    risk.erase("risk_reporting");
    EXPECT_EQ(parse_error(risk),
              msg("risk.risk_reporting is required (the live runners' reporter and the equity "
                  "start-up guard read it through AppConfig::risk_config)"));
}

TEST(RiskSchemaParse, C1TheReporterIsACarverOverAllBars) {
    nlohmann::json risk = risk_with({carver()});
    risk["risk_reporting"]["window"] = "trimmed";
    EXPECT_EQ(parse_error(risk),
              msg("risk.risk_reporting.type must be \"carver\" and window \"all_bars\" on day "
                  "one (HD Q5)"));
}

TEST(RiskSchemaParse, C1AReporterThatHasDriftedFromTheGateIsAnError) {
    nlohmann::json risk = risk_with({carver()});
    risk["risk_reporting"]["max_correlation"] = 0.84;
    EXPECT_EQ(parse_error(risk),
              msg("risk.risk_reporting.max_correlation = 0.84 differs from "
                  "risk.modules[0].max_correlation = 0.85; on day one the reporter mirrors the "
                  "gate (HD Q5)"));

    // With no carver module the reporter is free: the book is measured but not cut, which
    // is what commit 8 does to EQUITY_MR.
    nlohmann::json none_book = risk_with({none_module()});
    none_book["risk_reporting"]["max_correlation"] = 0.84;
    EXPECT_EQ(parse_error(none_book), "");
}

// ===== The sleeve block =====

TEST(RiskSchemaParse, ASleeveKeyMustNameAStrategyOfThisBook) {
    nlohmann::json sleeves = {
        {"TREND_FOLLOWING_SLOW", nlohmann::json::array({constant_scale_module()})}};
    EXPECT_EQ(parse_error(risk_with({carver()}), sleeves),
              msg("sleeve_risk_modules.TREND_FOLLOWING_SLOW does not name a strategy of this "
                  "portfolio"));
}

TEST(RiskSchemaParse, ASleeveBlockParsesIntoItsOwnModules) {
    nlohmann::json sleeves = {
        {"TREND_FOLLOWING", nlohmann::json::array({constant_scale_module("fast_cut")})}};
    auto r = parse_risk_schema(risk_with({carver()}), sleeves, one_sleeve(), kP);
    ASSERT_TRUE(r.is_ok()) << (r.is_error() ? r.error()->what() : "");
    ASSERT_EQ(r.value().sleeves.size(), 1u);
    const auto& mods = r.value().sleeves.at("TREND_FOLLOWING");
    ASSERT_EQ(mods.size(), 1u);
    EXPECT_EQ(mods[0].id, "fast_cut");
    EXPECT_EQ(mods[0].type, "constant_scale");
    EXPECT_FALSE(r.value().is_none());
}

// ===== Round trips =====

TEST(RiskSchemaRoundTrip, ToJsonReparsesIntoAnEqualSchemaForEveryModuleType) {
    nlohmann::json modules = nlohmann::json::array({carver(), warn_module(), refuse_module(),
                                                    constant_scale_module()});
    nlohmann::json sleeves = {
        {"TREND_FOLLOWING", nlohmann::json::array({constant_scale_module("sleeve_cut")})}};
    auto first = parse_risk_schema(risk_with(modules), sleeves, one_sleeve(), kP);
    ASSERT_TRUE(first.is_ok()) << (first.is_error() ? first.error()->what() : "");

    const nlohmann::json emitted = first.value().to_json();
    auto second = parse_risk_schema(emitted, first.value().sleeves_to_json(), one_sleeve(), kP);
    ASSERT_TRUE(second.is_ok()) << (second.is_error() ? second.error()->what() : "");
    EXPECT_EQ(second.value().to_json(), emitted);
    EXPECT_EQ(second.value().sleeves_to_json(), first.value().sleeves_to_json());

    // Adversary E3: a module object must not carry capital or version. They are the
    // runner's to set, and a stored copy of them is a second place they can disagree.
    for (const auto& module : emitted.at("modules")) {
        EXPECT_FALSE(module.contains("capital")) << module.at("id");
        EXPECT_FALSE(module.contains("version")) << module.at("id");
    }
}

TEST(RiskSchemaRoundTrip, ANoneModuleRoundTripsWithItsAttribution) {
    auto first = parse_risk_schema(risk_with({none_module()}), nlohmann::json(), one_sleeve(), kP);
    ASSERT_TRUE(first.is_ok());
    auto second = parse_risk_schema(first.value().to_json(), nlohmann::json(), one_sleeve(), kP);
    ASSERT_TRUE(second.is_ok()) << (second.is_error() ? second.error()->what() : "");
    const auto* n = std::get_if<NoneModuleConfig>(&second.value().portfolio[0].params);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->ruled_by, "HD");
    EXPECT_EQ(n->ruled_on, "2026-09-18");
}

// ===== make_none_module and make_risk_module =====

TEST(MakeNoneModule, RunsTheSameChecksAConfigNoneRuns) {
    auto ok = make_none_module("harness measures the strategy alone", "HD", "2026-09-18");
    ASSERT_TRUE(ok.is_ok()) << (ok.is_error() ? ok.error()->what() : "");
    EXPECT_EQ(ok.value().type, "none");
    EXPECT_TRUE(ok.value().terms().empty());

    EXPECT_TRUE(make_none_module("", "HD", "2026-09-18").is_error())
        << "an in-code none must carry a reason, exactly as a config one must";
    EXPECT_TRUE(make_none_module("why", "HD", "yesterday").is_error());
}

TEST(MakeRiskModule, BuildsTheShippedModuleForEachTypeAndNothingForNone) {
    auto schema = parse_risk_schema(
        risk_with({carver(), warn_module(), refuse_module(), constant_scale_module()}),
        nlohmann::json(), one_sleeve(), kP);
    ASSERT_TRUE(schema.is_ok()) << (schema.is_error() ? schema.error()->what() : "");
    const char* expected_types[] = {"carver", "warn", "refuse", "constant_scale"};
    size_t i = 0;
    for (const auto& module_config : schema.value().portfolio) {
        auto module = make_risk_module(module_config, Decimal(500000.0));
        ASSERT_TRUE(module.is_ok()) << module_config.id;
        ASSERT_NE(module.value(), nullptr) << module_config.id;
        EXPECT_EQ(module.value()->type(), expected_types[i]) << module_config.id;
        EXPECT_EQ(module.value()->id(), module_config.id);
        EXPECT_EQ(module.value()->terms(), module_config.terms())
            << module_config.id
            << ": the validator's view of a module's terms must match the module's own, or S8 "
               "protects a chain the loop does not have";
        ++i;
    }

    auto none_schema = parse_risk_schema(risk_with({none_module()}), nlohmann::json(),
                                         one_sleeve(), kP);
    ASSERT_TRUE(none_schema.is_ok());
    auto nothing = make_risk_module(none_schema.value().portfolio[0], Decimal(1.0));
    ASSERT_TRUE(nothing.is_ok());
    EXPECT_EQ(nothing.value(), nullptr) << "`none` is an assignment that builds nothing";
}

TEST(MakeRiskModule, TheCarverModuleCarriesTheBooksCapitalAndItsSevenValues) {
    auto schema = parse_risk_schema(risk_with({carver()}), nlohmann::json(), one_sleeve(), kP);
    ASSERT_TRUE(schema.is_ok());
    auto module = make_risk_module(schema.value().portfolio[0], Decimal(500000.0));
    ASSERT_TRUE(module.is_ok());
    auto* built = dynamic_cast<CarverRiskModule*>(module.value().get());
    ASSERT_NE(built, nullptr);
    const RiskConfig& c = built->manager().get_config();
    EXPECT_DOUBLE_EQ(c.var_limit, 0.25);
    EXPECT_DOUBLE_EQ(c.jump_risk_limit, 0.05);
    EXPECT_DOUBLE_EQ(c.max_correlation, 0.85);
    EXPECT_DOUBLE_EQ(c.max_gross_leverage, 4.0);
    EXPECT_DOUBLE_EQ(c.max_net_leverage, 2.0);
    EXPECT_DOUBLE_EQ(c.confidence_level, 0.99);
    EXPECT_EQ(c.lookback_period, 252);
    EXPECT_DOUBLE_EQ(c.capital.as_double(), 500000.0);
    EXPECT_EQ(built->min_gate_dates(), 21);
}

// ===== The tracked examples parse =====

TEST(TrackedRiskModuleExamples, EveryExampleLoadsThroughTheParserItDocuments) {
    namespace fs = std::filesystem;
    fs::path dir = fs::path(__FILE__).parent_path().parent_path().parent_path() /
                   "config_template" / "examples" / "risk_modules";
    if (!fs::exists(dir)) {
        fs::path walk = fs::current_path();
        for (int i = 0; i < 8 && !walk.empty(); ++i) {
            if (fs::exists(walk / "config_template" / "examples" / "risk_modules")) {
                dir = walk / "config_template" / "examples" / "risk_modules";
                break;
            }
            walk = walk.parent_path();
        }
    }
    ASSERT_TRUE(fs::exists(dir)) << "config_template/examples/risk_modules not found; this test "
                                    "must not skip";

    int checked = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.size() < 11 || name.substr(name.size() - 10) != ".risk.json") continue;
        std::ifstream in(entry.path());
        const auto risk = nlohmann::json::parse(in);
        // refuse_on_condition's module can REFUSE, so it needs a one-sleeve book; the
        // others are indifferent.
        auto r = parse_risk_schema(risk, nlohmann::json(), one_sleeve(), "EXAMPLE");
        EXPECT_TRUE(r.is_ok()) << name << ": " << (r.is_error() ? r.error()->what() : "");
        ++checked;
    }
    EXPECT_EQ(checked, 5) << "one example per module type, plus the two-module example";

    // The sleeve example is a portfolio.json, so it is checked through its own block.
    std::ifstream in(dir / "sleeve_assignment.portfolio.json");
    const auto portfolio = nlohmann::json::parse(in);
    std::ifstream risk_in(dir / "constant_scale.risk.json");
    const auto risk = nlohmann::json::parse(risk_in);
    auto r = parse_risk_schema(risk, portfolio.at("sleeve_risk_modules"),
                               portfolio.at("strategies"), "EXAMPLE");
    EXPECT_TRUE(r.is_ok()) << (r.is_error() ? r.error()->what() : "");
    EXPECT_EQ(r.value().sleeves.size(), 1u);
}

// ===== C-2, R10, R11 (T-6b commit 7d) =====

// A carver module divides by RiskConfig::capital and ignores RiskContext::capital by design,
// and make_risk_module hands every module the PORTFOLIO's capital. On a 30 % sleeve its
// max_gross_leverage 4.0 would therefore be 13.3x of the sleeve's own money.
TEST(RiskSchemaRulesT6b, CarverIsRefusedAtSleeveScope) {
    const std::string e = parse_error(risk_with({none_module()}),
                                      {{"TREND_FOLLOWING", {carver("sleeve_carver")}}});
    EXPECT_EQ(e, msg("sleeve_risk_modules.TREND_FOLLOWING[0] is type \"carver\", which is only "
                     "valid at portfolio scope: the Carver gate divides by the portfolio's "
                     "capital, so at sleeve scope its leverage limits would be read against the "
                     "whole book's money"));
}

TEST(RiskSchemaRulesT6b, CarverIsAcceptedAtPortfolioScope) {
    EXPECT_EQ(parse_error(risk_with({carver()})), "");
}

// R10. S3 ties attribution to the literal type "none", so a book whose only module is a WARN
// is just as ungated as a `none` book and used to carry nobody's name.
TEST(RiskSchemaRulesT6b, ABookWithNoCarverNeedsAnAttribution) {
    EXPECT_EQ(parse_error(risk_with({warn_module()})),
              msg("risk._ruled_by is required on a book no module of which is a \"carver\": "
                  "nothing here can cut this book, and that has to be somebody's decision rather "
                  "than an omission"));
    // A constant_scale of 1.0 passes every range rule and cuts nothing: same requirement.
    nlohmann::json one = constant_scale_module();
    one["scale"] = 1.0;
    EXPECT_EQ(parse_error(risk_with({one})),
              msg("risk._ruled_by is required on a book no module of which is a \"carver\": "
                  "nothing here can cut this book, and that has to be somebody's decision rather "
                  "than an omission"));
}

TEST(RiskSchemaRulesT6b, TheAttributionSatisfiesR10AndIsDateChecked) {
    nlohmann::json r = risk_with({warn_module()});
    r["_ruled_by"] = "HD";
    r["_ruled_on"] = "2026-09-18";
    EXPECT_EQ(parse_error(r), "");
    r["_ruled_on"] = "18/09/2026";
    EXPECT_EQ(parse_error(r), msg("risk._ruled_on must be YYYY-MM-DD, got \"18/09/2026\""));
    r["_ruled_on"] = "2026-09-18";
    r["_ruled_by"] = "";
    EXPECT_EQ(parse_error(r),
              msg("risk._ruled_by is required on a book no module of which is a \"carver\": "
                  "nothing here can cut this book, and that has to be somebody's decision rather "
                  "than an omission"));
}

// A lone `none` already carries all three fields on the module itself, so R10 exempts it --
// otherwise the same decision would have to be written down twice.
TEST(RiskSchemaRulesT6b, ALoneNoneIsExemptFromR10) {
    EXPECT_EQ(parse_error(risk_with({none_module()})), "");
}

// A carver anywhere in the chain satisfies R10, including one on a sleeve... which is now
// refused, so the only way to satisfy it is a portfolio carver. Pinned so the interaction of
// C-2 and R10 is deliberate rather than accidental.
TEST(RiskSchemaRulesT6b, ACarverAtPortfolioScopeSatisfiesR10ForTheWholeBook) {
    EXPECT_EQ(parse_error(risk_with({carver(), warn_module()})), "");
}

// R10's other half: a condition that can never hold is furniture, and says why it is listed.
TEST(RiskSchemaRulesT6b, ANeverConditionNeedsAReason) {
    nlohmann::json never = warn_module();
    // No threshold: a separate rule already refuses one on a "never" condition.
    never["condition"] = {{"kind", "never"}};
    nlohmann::json r = risk_with({carver(), never});
    EXPECT_EQ(parse_error(r),
              msg("risk.modules[1] has condition kind \"never\", so it can never fire; say why "
                  "it is listed in a non-empty _never_reason"));
    never["_never_reason"] = "kept so the shape is exercised by the loop tests";
    EXPECT_EQ(parse_error(risk_with({carver(), never})), "");
}

// ===== T-6b-fix F2: lookback_unit says what the code does =====

// The Carver window is capped at lookback_period distinct DATES and nothing reads the key. A
// risk.json migrated before T-6b commit 9 says "bars", which no longer describes the code: it is
// a load error that names the fix, never a synonym (T-6b INTERIM ADVERSARIAL B-2).
TEST(RiskSchemaRulesT6bFix, ABarsUnitIsRefusedAndTheErrorNamesTheMigration) {
    nlohmann::json c = carver();
    c["lookback_unit"] = "bars";
    EXPECT_EQ(parse_error(risk_with({c})),
              msg("risk.modules[0].lookback_unit is \"bars\", but the Carver window is capped at "
                  "lookback_period distinct DATES and nothing reads a bar count: write \"dates\" "
                  "(python3 scripts/migrate_risk_json.py <config_dir> --in-place upgrades the "
                  "file)"));
    EXPECT_EQ(parse_error(risk_with({carver()})), "") << "\"dates\" loads";
}

// R12 (B-5): F5 needs kF5MinGateDates COMPLETE dates to engage, and lookback_period counts
// DISTINCT dates, so a period below the floor could never engage it (the gate would read the
// zero-filled window for ever). The boundary, both sides; the reporter mirrors the gate (C1).
TEST(RiskSchemaRulesT6bFix, AWindowBelowTheSparseDateFloorIsALoadError) {
    auto with_window = [](int dates) {
        nlohmann::json c = carver();
        c["lookback_period"] = dates;
        nlohmann::json r = risk_with({c});
        r["risk_reporting"]["lookback_period"] = dates;
        return r;
    };
    EXPECT_EQ(parse_error(with_window(119)),
              msg("risk.modules[0].lookback_period (119 dates) is below the sparse-date filter's "
                  "floor of 120 complete dates, so the filter could never engage and the gate "
                  "would always read the zero-filled window"));
    EXPECT_EQ(parse_error(with_window(120)), "");
    EXPECT_EQ(CarverRiskModule::kF5MinGateDates, 120u) << "the rule reads the module's floor";
}

// Every shipped book carries 252 dates: R12 is a rule about configs nobody has written, not a
// change to the ones that exist.
TEST(RiskSchemaRulesT6bFix, TheShippedWindowIsAboveTheSparseDateFloor) {
    nlohmann::json c = carver();
    EXPECT_EQ(c.at("lookback_period").get<int>(), 252);
    EXPECT_EQ(c.at("lookback_unit").get<std::string>(), "dates");
    EXPECT_GE(static_cast<size_t>(c.at("lookback_period").get<int>()),
              CarverRiskModule::kF5MinGateDates);
    EXPECT_EQ(parse_error(risk_with({c})), "");
}

// ===== The three tracked books, and what each one is assigned (T-6b commit 8) =====

// HD's ruling of 2026-09-18: "No risk module on the EQUITY_MR portfolio book (assignment is per
// portfolio; other equity books choose their own)." This reads the TRACKED config_template tree
// rather than restating its values, so the assignment cannot drift from the file that ships.
TEST(TrackedPortfolioRiskAssignment, EachBookIsAssignedWhatHDRuled) {
    namespace fs = std::filesystem;
    fs::path root = fs::path(__FILE__).parent_path().parent_path().parent_path();
    if (!fs::exists(root / "config_template" / "portfolios")) {
        fs::path walk = fs::current_path();
        for (int i = 0; i < 8 && !walk.empty(); ++i) {
            if (fs::exists(walk / "config_template" / "portfolios")) {
                root = walk;
                break;
            }
            walk = walk.parent_path();
        }
    }
    const fs::path dir = root / "config_template" / "portfolios";
    ASSERT_TRUE(fs::exists(dir)) << "config_template/portfolios not found";

    auto modules_of = [&](const char* book) {
        std::ifstream in(dir / book / "risk.json");
        EXPECT_TRUE(in.good()) << book;
        return nlohmann::json::parse(in);
    };

    // The two futures books keep the full four-term Carver gate.
    for (const char* book : {"conservative", "base"}) {
        const auto risk = modules_of(book);
        ASSERT_EQ(risk.at("modules").size(), 1u) << book;
        EXPECT_EQ(risk.at("modules")[0].at("type"), "carver") << book;
    }

    // EQUITY_MR runs NO risk layer, and says who ruled it and when.
    const auto eq = modules_of("equity_mr");
    ASSERT_EQ(eq.at("modules").size(), 1u);
    EXPECT_EQ(eq.at("modules")[0].at("type"), "none");
    EXPECT_EQ(eq.at("modules")[0].at("_ruled_by"), "HD");
    EXPECT_EQ(eq.at("modules")[0].at("_ruled_on"), "2026-09-18");
    EXPECT_FALSE(eq.at("modules")[0].at("_reason").get<std::string>().empty());

    // ...but it is still MEASURED. The reporter is untouched (HD Q5, RA-01), so every risk
    // column of live_results and of the email still carries a number. A `none` assignment that
    // also dropped the reporter would silently blank those columns.
    ASSERT_TRUE(eq.contains("risk_reporting"));
    EXPECT_EQ(eq.at("risk_reporting").at("type"), "carver");
    EXPECT_EQ(eq.at("risk_reporting").at("window"), "all_bars");
    EXPECT_DOUBLE_EQ(eq.at("risk_reporting").at("var_limit").get<double>(), 0.25);
    EXPECT_DOUBLE_EQ(eq.at("risk_reporting").at("jump_risk_limit").get<double>(), 0.08);
    EXPECT_DOUBLE_EQ(eq.at("risk_reporting").at("max_correlation").get<double>(), 0.7);
    EXPECT_EQ(eq.at("risk_reporting").at("lookback_period").get<int>(), 252);

    // Every one of the three still parses, so `none` is a shape the loader accepts on a real
    // book and not only in the examples.
    for (const char* book : {"conservative", "base", "equity_mr"}) {
        std::ifstream pin(dir / book / "portfolio.json");
        ASSERT_TRUE(pin.good()) << book;
        const auto portfolio = nlohmann::json::parse(pin);
        const auto sleeves = portfolio.contains("sleeve_risk_modules")
                                 ? portfolio.at("sleeve_risk_modules")
                                 : nlohmann::json();
        auto r = parse_risk_schema(modules_of(book), sleeves, portfolio.at("strategies"), book);
        EXPECT_TRUE(r.is_ok()) << book << ": " << (r.is_error() ? r.error()->what() : "");
    }
}

// A `none` book builds a PortfolioManager that runs no gate at all -- rather than one that
// throws on an empty module list, which is what an OMISSION does.
TEST(TrackedPortfolioRiskAssignment, ANoneAssignmentBuildsAManagerThatRunsNoGate) {
    PortfolioConfig pc{100000.0, 0.0, 1.0, 0.0, /*optimization=*/false};
    pc.risk_config.capital = 100000.0;
    pc.risk_modules = {trade_ngin::testing::test_none_module("no_portfolio_risk")};
    PortfolioManager pm(pc, "PM_NONE_ASSIGNMENT");
    EXPECT_TRUE(pm.last_risk_decisions().empty());
    // The same config with the list EMPTY is a forgotten line, and throws.
    PortfolioConfig omitted = pc;
    omitted.risk_modules.clear();
    EXPECT_THROW(PortfolioManager(omitted, "PM_OMITTED"), std::invalid_argument);
}
