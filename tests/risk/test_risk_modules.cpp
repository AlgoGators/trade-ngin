// Risk modules, one at a time (no PortfolioManager): the Carver module against a
// bare RiskManager, the plug-and-play modules (constant scale, warn, refuse on a
// condition) and the decisions JSON helper.

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/test_base.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/risk/basic_risk_modules.hpp"
#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/risk/risk_module.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

using Book = std::unordered_map<std::string, Position>;

Timestamp day(int d) {
    return Timestamp(std::chrono::seconds(1767225600LL + 86400LL * d));
}

Bar make_bar(const std::string& symbol, int d, double close) {
    Bar b;
    b.symbol = symbol;
    b.timestamp = day(d);
    b.open = b.high = b.low = b.close = Decimal(close);
    b.volume = 1000.0;
    return b;
}

Position make_pos(const std::string& symbol, double qty, double price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.last_update = day(0);
    return p;
}

// 3 symbols x 30 dates. TSTB is exactly 2 x TSTA, so their returns are identical and
// their correlation is 1; TSTC moves on its own.
std::vector<Bar> correlated_window() {
    std::vector<Bar> w;
    for (int d = 0; d < 30; ++d) {
        const double a = 100.0 * (1.0 + 0.02 * std::sin(0.9 * d));
        w.push_back(make_bar("TSTA", d, a));
        w.push_back(make_bar("TSTB", d, 2.0 * a));
        w.push_back(make_bar("TSTC", d, 50.0 * (1.0 + 0.03 * std::cos(1.7 * d + 0.4))));
    }
    return w;
}

// Every limit differs from RiskConfig{}'s default, so a module built from the defaults
// cannot reproduce the result.
RiskConfig tight_config() {
    RiskConfig c;
    c.var_limit = 0.5;
    c.jump_risk_limit = 0.5;
    c.max_correlation = 0.5;
    c.max_gross_leverage = 0.3;
    c.max_net_leverage = 0.25;
    c.capital = Decimal(100000.0);
    c.lookback_period = 1000;
    return c;
}

Book correlated_book() {
    return {{"TSTA", make_pos("TSTA", 200.0, 100.0)},
            {"TSTB", make_pos("TSTB", 60.0, 200.0)},
            {"TSTC", make_pos("TSTC", -150.0, 50.0)}};
}

bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

RiskContext lap_ctx(int lap) {
    RiskContext c;
    c.phase = RiskPhase::LAP;
    c.lap = lap;
    c.scope = RiskScope::PORTFOLIO;
    c.scope_id = "PM";
    c.portfolio_id = "PM";
    return c;
}

void init_console_logger() {
    LoggerConfig lc;
    lc.destination = LogDestination::CONSOLE;
    lc.min_level = LogLevel::INFO;
    lc.include_timestamp = false;
    Logger::instance().initialize(lc);
}

}  // namespace

// ===== CarverRiskModule =====

TEST(CarverRiskModuleTest, EvaluateBitwiseEqualsBareRiskManager) {
    const RiskConfig cfg = tight_config();
    std::vector<Bar> window = correlated_window();
    const Book book = correlated_book();

    RiskManager bare(cfg);
    auto ref = bare.process_positions(book, bare.create_market_data(window), {});
    ASSERT_TRUE(ref.is_ok());
    const RiskResult& r = ref.value();
    // The case discriminates: at least two multipliers bind.
    int binding = (r.portfolio_multiplier < 1.0) + (r.jump_multiplier < 1.0) +
                  (r.correlation_multiplier < 1.0) + (r.leverage_multiplier < 1.0);
    ASSERT_GE(binding, 2) << "corr=" << r.correlation_multiplier << " lev=" << r.leverage_multiplier;

    // The module owns its window: one on_bars with the whole window (lookback 1000 >= 90 bars).
    CarverRiskModule carver("carver", cfg);
    RiskContext ctx = lap_ctx(1);
    carver.on_bars(window, ctx);
    ASSERT_EQ(carver.window().size(), window.size());
    auto got = carver.evaluate(book, ctx);
    ASSERT_TRUE(got.is_ok());
    const RiskDecision& d = got.value();
    ASSERT_TRUE(d.metrics.has_value());
    const RiskResult& m = *d.metrics;

    EXPECT_EQ(m.risk_exceeded, r.risk_exceeded);
    EXPECT_TRUE(same_bits(m.recommended_scale, r.recommended_scale));
    EXPECT_TRUE(same_bits(m.portfolio_var, r.portfolio_var));
    EXPECT_TRUE(same_bits(m.jump_risk, r.jump_risk));
    EXPECT_TRUE(same_bits(m.correlation_risk, r.correlation_risk));
    EXPECT_TRUE(same_bits(m.gross_leverage, r.gross_leverage));
    EXPECT_TRUE(same_bits(m.net_leverage, r.net_leverage));
    EXPECT_TRUE(same_bits(m.max_portfolio_risk, r.max_portfolio_risk));
    EXPECT_TRUE(same_bits(m.max_jump_risk, r.max_jump_risk));
    EXPECT_TRUE(same_bits(m.max_leverage_risk, r.max_leverage_risk));
    EXPECT_TRUE(same_bits(m.portfolio_multiplier, r.portfolio_multiplier));
    EXPECT_TRUE(same_bits(m.jump_multiplier, r.jump_multiplier));
    EXPECT_TRUE(same_bits(m.correlation_multiplier, r.correlation_multiplier));
    EXPECT_TRUE(same_bits(m.leverage_multiplier, r.leverage_multiplier));

    EXPECT_EQ(d.action, RiskAction::SCALE);
    EXPECT_TRUE(same_bits(d.scale, r.recommended_scale));
    EXPECT_EQ(d.module_id, "carver");
    EXPECT_FALSE(d.blind);
    EXPECT_TRUE(d.detail.empty());
}

TEST(CarverRiskModuleTest, ActionIsKeyedOnRiskExceeded) {
    RiskResult exceeded;
    exceeded.risk_exceeded = true;
    exceeded.recommended_scale = 0.85;
    auto d = CarverRiskModule::to_decision(exceeded, "carver");
    EXPECT_EQ(d.action, RiskAction::SCALE);
    EXPECT_TRUE(same_bits(d.scale, 0.85));
    ASSERT_TRUE(d.metrics.has_value());
    EXPECT_TRUE(same_bits(d.metrics->recommended_scale, 0.85));

    RiskResult fine;  // risk_exceeded false, scale 1.0
    d = CarverRiskModule::to_decision(fine, "carver");
    EXPECT_EQ(d.action, RiskAction::NONE);
    EXPECT_EQ(d.scale, 1.0);
    EXPECT_TRUE(d.metrics.has_value());

    // NaN < 1.0 is false, so RiskManager leaves risk_exceeded false: NONE, never SCALE(NaN)
    // (Decimal(NaN) throws, which would be a new ERROR line and a skipped scale).
    RiskResult nan_scale;
    nan_scale.recommended_scale = std::numeric_limits<double>::quiet_NaN();
    d = CarverRiskModule::to_decision(nan_scale, "carver");
    EXPECT_EQ(d.action, RiskAction::NONE) << "a NaN scale must map to NONE";
    EXPECT_EQ(d.scale, 1.0);

    RiskResult zero;
    zero.risk_exceeded = true;
    zero.recommended_scale = 0.0;
    d = CarverRiskModule::to_decision(zero, "carver");
    EXPECT_EQ(d.action, RiskAction::SCALE);
    EXPECT_EQ(d.scale, 0.0);
}

TEST(CarverRiskModuleTest, RegistersNoComponentAndLogsNothingInCtor) {
    init_console_logger();
    Logger::register_component("Sentinel");
    ::testing::internal::CaptureStdout();
    {
        CarverRiskModule carver("carver", tight_config());
        INFO("probe");
    }
    const std::string out = ::testing::internal::GetCapturedStdout();
    // Nothing printed by construction, and the only registration was the RiskManager's.
    EXPECT_EQ(out, "[INFO] [RiskManager] probe\n");
}

TEST(CarverRiskModuleTest, DescribeCarriesTheConfigAndTerms) {
    CarverRiskModule carver("carver", tight_config());
    auto j = carver.describe();
    EXPECT_EQ(j["id"], "carver");
    EXPECT_EQ(j["type"], "carver");
    EXPECT_EQ(j["terms"], nlohmann::json({"composition", "magnitude"}));
    EXPECT_EQ(j["config"], tight_config().to_json());
    // SCALE on a lap; WARN at the post-rounding point when the shipped book is over its leverage
    // limit (T-6b-fix F5, the written policy). Never REFUSE: a failing carver is not a gatekeeper.
    EXPECT_EQ(carver.capabilities(), (std::set<RiskAction>{RiskAction::SCALE, RiskAction::WARN}));
}

TEST(CarverRiskModuleTest, BeginRebalanceResetsFlagAndLevelNotWindow) {
    CarverRiskModule carver("carver", tight_config());
    EXPECT_FALSE(carver.appended_this_rebalance());
    const std::vector<Bar> bars = {make_bar("TSTA", 0, 100.0), make_bar("TSTA", 1, 101.0)};
    RiskContext ctx = lap_ctx(1);
    carver.on_bars(bars, ctx);
    EXPECT_TRUE(carver.appended_this_rebalance());
    auto d = carver.evaluate({{"TSTA", make_pos("TSTA", 1000.0, 100.0)}}, ctx);
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(d.value().action, RiskAction::SCALE);  // 1.0x gross against a 0.3x cap
    EXPECT_EQ(carver.last_requested(), d.value().scale);

    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    EXPECT_FALSE(carver.appended_this_rebalance());
    EXPECT_EQ(carver.applied_level(), 1.0);
    EXPECT_EQ(carver.last_requested(), 1.0);
    // The window spans rebalances: begin_rebalance must not clear it.
    ASSERT_EQ(carver.window().size(), 2u);
    EXPECT_EQ(carver.window()[1].close.raw_value(), Decimal(101.0).raw_value());
}

// ===== ConstantScaleRiskModule =====

TEST(ConstantScaleRiskModuleTest, ScaleBelowOneIsScaleOneIsNoneOutOfRangeThrows) {
    const Book book = {{"X", make_pos("X", 1.0, 1.0)}};
    ConstantScaleRiskModule half("half", 0.5);
    auto d = half.evaluate(book, lap_ctx(1));
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value().action, RiskAction::SCALE);
    EXPECT_EQ(d.value().scale, 0.5);
    EXPECT_EQ(d.value().module_id, "half");

    ConstantScaleRiskModule one("one", 1.0);
    d = one.evaluate(book, lap_ctx(1));
    EXPECT_EQ(d.value().action, RiskAction::NONE);
    EXPECT_EQ(d.value().scale, 1.0);

    ConstantScaleRiskModule zero("zero", 0.0);
    EXPECT_EQ(zero.evaluate(book, lap_ctx(1)).value().action, RiskAction::SCALE);

    EXPECT_THROW(ConstantScaleRiskModule("x", 1.5), std::invalid_argument);
    EXPECT_THROW(ConstantScaleRiskModule("x", -0.1), std::invalid_argument);
    EXPECT_THROW(ConstantScaleRiskModule("x", std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
}

TEST(ConstantScaleRiskModuleTest, OncePerRebalanceUnlessEveryLap) {
    const Book book = {{"X", make_pos("X", 1.0, 1.0)}};
    ConstantScaleRiskModule once("once", 0.8);
    ConstantScaleRiskModule every("every", 0.8, /*every_lap=*/true);
    EXPECT_EQ(once.evaluate(book, lap_ctx(1)).value().action, RiskAction::SCALE);
    EXPECT_EQ(once.evaluate(book, lap_ctx(2)).value().action, RiskAction::NONE);
    EXPECT_EQ(every.evaluate(book, lap_ctx(2)).value().action, RiskAction::SCALE);
    RiskContext sleeve;
    sleeve.phase = RiskPhase::SLEEVE;
    sleeve.lap = 0;
    EXPECT_EQ(once.evaluate(book, sleeve).value().action, RiskAction::SCALE);
    EXPECT_EQ(once.describe()["params"]["every_lap"], false);
    EXPECT_EQ(every.describe()["type"], "constant_scale");
    EXPECT_EQ(every.describe()["terms"], nlohmann::json({"magnitude"}));
}

// ===== WarnRiskModule / RefuseOnConditionRiskModule =====

namespace {

struct ConditionCase {
    RiskCondition condition;
    Book book;
    RiskContext ctx;
    bool expect_holds;
};

std::vector<ConditionCase> condition_cases() {
    const Book two = {{"A", make_pos("A", 2.0, 1.0)}, {"B", make_pos("B", -7.5, 1.0)}};
    const Book one_zero = {{"A", make_pos("A", 0.0, 1.0)}, {"B", make_pos("B", 3.0, 1.0)}};
    RiskContext post = lap_ctx(4);
    post.phase = RiskPhase::POST_ROUNDING;
    using K = RiskCondition::Kind;
    return {
        {{K::ALWAYS, 0.0}, two, lap_ctx(1), true},
        {{K::NEVER, 0.0}, two, lap_ctx(1), false},
        {{K::LAP_AT_LEAST, 3.0}, two, lap_ctx(2), false},
        {{K::LAP_AT_LEAST, 3.0}, two, lap_ctx(3), true},
        {{K::LAP_AT_LEAST, 3.0}, two, post, false},  // LAP phase only
        {{K::NONZERO_POSITIONS_ABOVE, 1.0}, two, lap_ctx(1), true},
        {{K::NONZERO_POSITIONS_ABOVE, 1.0}, one_zero, lap_ctx(1), false},
        {{K::MAX_ABS_QUANTITY_ABOVE, 7.0}, two, lap_ctx(1), true},  // |-7.5| > 7
        {{K::MAX_ABS_QUANTITY_ABOVE, 7.5}, two, lap_ctx(1), false},
    };
}

}  // namespace

TEST(WarnRiskModuleTest, WarnsOnlyWhenConditionHolds) {
    for (const auto& c : condition_cases()) {
        WarnRiskModule warn("w", c.condition, "because");
        auto d = warn.evaluate(c.book, c.ctx);
        ASSERT_TRUE(d.is_ok());
        SCOPED_TRACE(c.condition.to_json().dump() + " lap " + std::to_string(c.ctx.lap));
        EXPECT_EQ(d.value().action, c.expect_holds ? RiskAction::WARN : RiskAction::NONE);
        EXPECT_EQ(d.value().reason, c.expect_holds ? "because" : "");
        EXPECT_EQ(d.value().module_id, "w");
    }
    WarnRiskModule warn("w", {RiskCondition::Kind::LAP_AT_LEAST, 3.0}, "late");
    EXPECT_EQ(warn.describe().dump(),
              R"({"id":"w","params":{"condition":{"kind":"lap_at_least","threshold":3.0},)"
              R"("reason":"late"},"terms":["custom"],"type":"warn"})");
    EXPECT_EQ(warn.capabilities(), std::set<RiskAction>{RiskAction::WARN});
}

TEST(RefuseOnConditionRiskModuleTest, RefusesOnlyWhenConditionHolds) {
    for (const auto& c : condition_cases()) {
        RefuseOnConditionRiskModule refuse("r", c.condition, "stop");
        auto d = refuse.evaluate(c.book, c.ctx);
        ASSERT_TRUE(d.is_ok());
        SCOPED_TRACE(c.condition.to_json().dump() + " lap " + std::to_string(c.ctx.lap));
        EXPECT_EQ(d.value().action, c.expect_holds ? RiskAction::REFUSE : RiskAction::NONE);
        EXPECT_EQ(d.value().reason, c.expect_holds ? "stop" : "");
    }
    RefuseOnConditionRiskModule refuse("r", {RiskCondition::Kind::ALWAYS, 0.0}, "stop");
    EXPECT_EQ(refuse.describe()["type"], "refuse");
    EXPECT_EQ(refuse.capabilities(), std::set<RiskAction>{RiskAction::REFUSE});
}

// ===== build_risk_decisions_json =====

namespace {

nlohmann::json carver_module_json() {
    return nlohmann::json{{"id", "carver"},
                          {"type", "carver"},
                          {"terms", {"composition", "magnitude"}},
                          {"scope", "portfolio"},
                          {"scope_id", "PM"}};
}

RiskDecisionRecord make_record(const std::string& module, RiskAction requested, double scale,
                               RiskAction applied, double factor, const std::string& scope_id,
                               int lap) {
    RiskDecisionRecord r;
    r.phase = RiskPhase::LAP;
    r.lap = lap;
    r.scope = RiskScope::PORTFOLIO;
    r.scope_id = scope_id;
    r.module_id = module;
    r.requested.action = requested;
    r.requested.scale = scale;
    r.requested.module_id = module;
    r.applied_action = applied;
    r.applied_factor = Decimal(factor);
    return r;
}

}  // namespace

TEST(RiskDecisionsJsonTest, ShapeIsStableForAFixedRecordSet) {
    RiskDecisionRecord carver =
        make_record("carver", RiskAction::SCALE, 0.85, RiskAction::SCALE, 0.85, "PM", 1);
    RiskResult r;
    r.risk_exceeded = true;
    r.recommended_scale = 0.85;
    r.correlation_multiplier = 0.85;
    carver.requested.metrics = r;
    RiskDecisionRecord warn =
        make_record("w", RiskAction::WARN, 1.0, RiskAction::NONE, 1.0, "PM", 1);
    warn.requested.reason = "r";

    const std::string expected =
        R"({"decisions":[{"applied":{"action":"SCALE","factor":0.85,"factor_raw":85000000},)"
        R"("empty_book":false,"lap":1,"metrics":{"correlation_multiplier":0.85,)"
        R"("correlation_risk":0.0,"gross_leverage":0.0,"jump_multiplier":1.0,"jump_risk":0.0,)"
        R"("leverage_multiplier":1.0,"net_leverage":0.0,"portfolio_multiplier":1.0,)"
        R"("portfolio_var":0.0,"recommended_scale":0.85,"risk_exceeded":true},"module":"carver",)"
        R"("phase":"lap","requested":{"action":"SCALE","blind":false,"reason":"","scale":0.85},)"
        R"("scope":"portfolio","scope_id":"PM"},)"
        R"({"applied":{"action":"NONE","factor":1.0,"factor_raw":100000000},"empty_book":false,)"
        R"("lap":1,"module":"w","phase":"lap","requested":{"action":"WARN","blind":false,)"
        R"("reason":"r","scale":1.0},"scope":"portfolio","scope_id":"PM"}],)"
        R"("modules":[{"id":"carver","scope":"portfolio","scope_id":"PM",)"
        R"("terms":["composition","magnitude"],"type":"carver"}],)"
        R"("outcome":{"action":"SCALE","laps":1,"pinned_scopes":[],"refused":false}})";
    EXPECT_EQ(build_risk_decisions_json({carver_module_json()}, {carver, warn}).dump(), expected);
}

TEST(RiskDecisionsJsonTest, EmptyRecordsMeanNoDecisions) {
    EXPECT_EQ(build_risk_decisions_json({carver_module_json()}, {}).dump(),
              R"({"decisions":[],"modules":[{"id":"carver","scope":"portfolio","scope_id":"PM",)"
              R"("terms":["composition","magnitude"],"type":"carver"}],)"
              R"("outcome":{"action":"NONE","laps":0,"pinned_scopes":[],"refused":false}})");
}

TEST(RiskDecisionsJsonTest, OutcomeIsMostSevereApplied) {
    // A REFUSE that was requested but not applied does not make the outcome a refusal.
    std::vector<RiskDecisionRecord> records = {
        make_record("refuse", RiskAction::REFUSE, 1.0, RiskAction::NONE, 1.0, "PM", 2),
        make_record("carver", RiskAction::SCALE, 0.9, RiskAction::SCALE, 0.9, "PM", 3),
    };
    auto j = build_risk_decisions_json({}, records);
    EXPECT_EQ(j["outcome"]["action"], "SCALE");
    EXPECT_EQ(j["outcome"]["refused"], false);
    EXPECT_EQ(j["outcome"]["pinned_scopes"], nlohmann::json::array());
    EXPECT_EQ(j["outcome"]["laps"], 3);

    records.push_back(make_record("r2", RiskAction::REFUSE, 1.0, RiskAction::REFUSE, 1.0, "S1", 1));
    records.push_back(
        make_record("rep", RiskAction::REPLACE, 1.0, RiskAction::REPLACE, 1.0, "S0", 1));
    j = build_risk_decisions_json({}, records);
    EXPECT_EQ(j["outcome"]["action"], "REFUSE");
    EXPECT_EQ(j["outcome"]["refused"], true);
    EXPECT_EQ(j["outcome"]["pinned_scopes"], nlohmann::json({"S0", "S1"}));
}

TEST(RiskDecisionsJsonTest, ErrorAndEmptyBookRowsAreMarked) {
    RiskDecisionRecord empty = make_record("carver", RiskAction::NONE, 1.0, RiskAction::NONE, 1.0,
                                           "PM", 1);
    empty.empty_book = true;
    RiskDecisionRecord failed = make_record("carver", RiskAction::NONE, 1.0, RiskAction::NONE, 1.0,
                                            "PM", 2);
    failed.error = "boom";
    auto j = build_risk_decisions_json({}, {empty, failed});
    EXPECT_EQ(j["decisions"][0]["empty_book"], true);
    EXPECT_FALSE(j["decisions"][0].contains("error"));
    EXPECT_FALSE(j["decisions"][0].contains("metrics"));
    EXPECT_EQ(j["decisions"][1]["error"], "boom");
}

TEST(RiskModuleNamesTest, EveryEnumHasItsName) {
    EXPECT_STREQ(risk_action_name(RiskAction::NONE), "NONE");
    EXPECT_STREQ(risk_action_name(RiskAction::SCALE), "SCALE");
    EXPECT_STREQ(risk_action_name(RiskAction::WARN), "WARN");
    EXPECT_STREQ(risk_action_name(RiskAction::REFUSE), "REFUSE");
    EXPECT_STREQ(risk_action_name(RiskAction::REPLACE), "REPLACE");
    EXPECT_STREQ(risk_term_name(RiskTerm::COMPOSITION), "composition");
    EXPECT_STREQ(risk_term_name(RiskTerm::MAGNITUDE), "magnitude");
    EXPECT_STREQ(risk_term_name(RiskTerm::PATH), "path");
    EXPECT_STREQ(risk_term_name(RiskTerm::CUSTOM), "custom");
    EXPECT_STREQ(risk_scope_name(RiskScope::PORTFOLIO), "portfolio");
    EXPECT_STREQ(risk_scope_name(RiskScope::SLEEVE), "sleeve");
    EXPECT_STREQ(risk_phase_name(RiskPhase::REBALANCE_START), "rebalance_start");
    EXPECT_STREQ(risk_phase_name(RiskPhase::SLEEVE), "sleeve");
    EXPECT_STREQ(risk_phase_name(RiskPhase::LAP), "lap");
    EXPECT_STREQ(risk_phase_name(RiskPhase::POST_ROUNDING), "post_rounding");
}

// ===== The runtime `blind` flag (LEAD_RULINGS_C7 item 3) =====
//
// A measurement made on seven dates is not a measurement; schema 1 had no way to say so,
// and T-4 found the futures gate running on windows of about that size while every stored
// number looked like a real one. The module now counts the COMPLETE dates in its window --
// dates on which every symbol present in the window printed -- and marks the decision
// `blind` below min_gate_dates, or when there is no capital to divide by.
//
// It is DATA ONLY: it never changes the action or the scale, and nothing logs or stores it
// in T-6a (T-7 item 10 stores it). The last test here is the one that says so.

namespace {

/// `symbols` x `dates` bars, every symbol printing on every date.
std::vector<Bar> full_window(int dates, const std::vector<std::string>& symbols) {
    std::vector<Bar> w;
    for (int d = 0; d < dates; ++d) {
        for (size_t s = 0; s < symbols.size(); ++s) {
            w.push_back(make_bar(symbols[s], d, 100.0 + d + 3.0 * static_cast<double>(s)));
        }
    }
    return w;
}

}  // namespace

TEST(CarverBlindTest, CountsCompleteDatesNotBars) {
    RiskConfig cfg = tight_config();
    CarverRiskModule carver("carver", cfg, /*min_gate_dates=*/21);
    RiskContext ctx = lap_ctx(1);

    carver.on_bars(full_window(7, {"TSTA", "TSTB", "TSTC"}), ctx);
    EXPECT_EQ(carver.complete_dates_in_window(), 7)
        << "21 BARS over three symbols is seven dates: the count is of dates, not bars";

    CarverRiskModule wide("carver", cfg, 21);
    wide.on_bars(full_window(21, {"TSTA", "TSTB", "TSTC"}), ctx);
    EXPECT_EQ(wide.complete_dates_in_window(), 21);
}

TEST(CarverBlindTest, ADateOneSymbolMissedIsNotComplete) {
    RiskConfig cfg = tight_config();
    std::vector<Bar> window = full_window(21, {"TSTA", "TSTB"});
    // Drop TSTB's bar on the last date: 21 distinct dates, 20 complete ones.
    window.pop_back();
    CarverRiskModule carver("carver", cfg, 21);
    carver.on_bars(window, lap_ctx(1));
    EXPECT_EQ(carver.complete_dates_in_window(), 20)
        << "a date the whole universe did not print on cannot support a covariance";
}

TEST(CarverBlindTest, BlindBelowTheFloorAndSightedAtIt) {
    RiskConfig cfg = tight_config();
    const Book book = {{"TSTA", make_pos("TSTA", 10.0, 100.0)},
                       {"TSTB", make_pos("TSTB", 10.0, 103.0)}};
    RiskContext ctx = lap_ctx(1);

    CarverRiskModule short_window("carver", cfg, 21);
    short_window.on_bars(full_window(7, {"TSTA", "TSTB"}), ctx);
    auto few = short_window.evaluate(book, ctx);
    ASSERT_TRUE(few.is_ok());
    EXPECT_TRUE(few.value().blind) << "seven complete dates, floor 21";

    CarverRiskModule enough("carver", cfg, 21);
    enough.on_bars(full_window(21, {"TSTA", "TSTB"}), ctx);
    auto many = enough.evaluate(book, ctx);
    ASSERT_TRUE(many.is_ok());
    EXPECT_FALSE(many.value().blind) << "21 complete dates, floor 21";

    CarverRiskModule one_short("carver", cfg, 21);
    std::vector<Bar> window = full_window(21, {"TSTA", "TSTB"});
    window.pop_back();
    one_short.on_bars(window, ctx);
    auto twenty = one_short.evaluate(book, ctx);
    ASSERT_TRUE(twenty.is_ok());
    EXPECT_TRUE(twenty.value().blind) << "20 complete dates, floor 21";
}

TEST(CarverBlindTest, NoCapitalToDivideByIsAlsoBlind) {
    RiskConfig cfg = tight_config();
    cfg.capital = Decimal(0.0);
    CarverRiskModule carver("carver", cfg, 21);
    RiskContext ctx = lap_ctx(1);
    carver.on_bars(full_window(30, {"TSTA", "TSTB"}), ctx);
    auto d = carver.evaluate({{"TSTA", make_pos("TSTA", 10.0, 100.0)}}, ctx);
    ASSERT_TRUE(d.is_ok());
    EXPECT_TRUE(d.value().blind)
        << "every leverage and VaR figure the gate computes divides by capital";
}

TEST(CarverBlindTest, TheFlagIsInertTheResultIsBitwiseTheSameEitherWay) {
    // The same window and book, gated once with a floor it clears and once with a floor
    // it cannot: the decision's action, scale and every metric must be identical, and only
    // `blind` may differ. If the flag ever reaches the cut, this is what catches it.
    RiskConfig cfg = tight_config();
    const std::vector<Bar> window = correlated_window();
    const Book book = correlated_book();
    RiskContext ctx = lap_ctx(1);

    CarverRiskModule sighted("carver", cfg, /*min_gate_dates=*/3);
    sighted.on_bars(window, ctx);
    auto a = sighted.evaluate(book, ctx);
    ASSERT_TRUE(a.is_ok());

    CarverRiskModule blinded("carver", cfg, /*min_gate_dates=*/1000);
    blinded.on_bars(window, ctx);
    auto b = blinded.evaluate(book, ctx);
    ASSERT_TRUE(b.is_ok());

    EXPECT_FALSE(a.value().blind);
    EXPECT_TRUE(b.value().blind);
    EXPECT_EQ(a.value().action, b.value().action);
    EXPECT_TRUE(same_bits(a.value().scale, b.value().scale));
    ASSERT_TRUE(a.value().metrics.has_value());
    ASSERT_TRUE(b.value().metrics.has_value());
    EXPECT_EQ(a.value().metrics->risk_exceeded, b.value().metrics->risk_exceeded);
    EXPECT_TRUE(same_bits(a.value().metrics->recommended_scale,
                          b.value().metrics->recommended_scale));
    EXPECT_TRUE(same_bits(a.value().metrics->portfolio_multiplier,
                          b.value().metrics->portfolio_multiplier));
    EXPECT_TRUE(same_bits(a.value().metrics->correlation_multiplier,
                          b.value().metrics->correlation_multiplier));
    EXPECT_TRUE(same_bits(a.value().metrics->leverage_multiplier,
                          b.value().metrics->leverage_multiplier));
}

TEST(CarverBlindTest, TheBlindDecisionLogsNothingBeyondTheUnchangedResultLine) {
    // T-6a carries the flag and does not report it: no new log line, no stored column.
    init_console_logger();
    RiskConfig cfg = tight_config();
    CarverRiskModule carver("carver", cfg, /*min_gate_dates=*/1000);
    RiskContext ctx = lap_ctx(1);
    carver.on_bars(correlated_window(), ctx);

    ::testing::internal::CaptureStdout();
    auto d = carver.evaluate(correlated_book(), ctx);
    const std::string out = ::testing::internal::GetCapturedStdout();
    ASSERT_TRUE(d.is_ok());
    ASSERT_TRUE(d.value().blind);
    EXPECT_EQ(out.find("blind"), std::string::npos) << out;
    EXPECT_EQ(out.find("min_gate_dates"), std::string::npos) << out;
    EXPECT_NE(out.find("Risk management result:"), std::string::npos)
        << "the one line the gate has always printed must still be the only one";
}

// ===== T-6b commit 9: the risk-loop set (T-4f ARM 1) inside the module =====

namespace {

/// A window of `dates` dates x `symbols` symbols, every symbol printing on every date.
std::vector<Bar> dense_window(int dates, int symbols) {
    std::vector<Bar> w;
    for (int d = 0; d < dates; ++d) {
        for (int s = 0; s < symbols; ++s) {
            w.push_back(make_bar("SYM" + std::to_string(s), d,
                                 100.0 + 3.0 * s + 0.7 * d +
                                     2.0 * std::sin(0.3 * d + 0.5 * s)));
        }
    }
    return w;
}

}  // namespace

// THE window bug. The loop calls on_bars once per lap with the SAME bars; the old code appended
// every time and, with a BAR cap, evicted older dates until the window held three dates and
// |rho| was 1.0 by arithmetic. Now lap 1 appends and laps 2..n do not.
TEST(CarverArm1WindowTest, TheWindowIsAppendedOncePerRebalanceNotOncePerLap) {
    CarverRiskModule carver("carver", tight_config());
    const std::vector<Bar> one_date = {make_bar("TSTA", 0, 100.0), make_bar("TSTB", 0, 200.0)};

    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    for (int lap = 1; lap <= 5; ++lap) {
        RiskContext ctx = lap_ctx(lap);
        carver.on_bars(one_date, ctx);
    }
    EXPECT_EQ(carver.window().size(), 2u) << "five laps of the same two bars are two bars";
    EXPECT_EQ(carver.window_dates(), 1u);

    // A NEW rebalance appends again.
    carver.begin_rebalance(start);
    RiskContext ctx = lap_ctx(1);
    carver.on_bars({make_bar("TSTA", 1, 101.0), make_bar("TSTB", 1, 202.0)}, ctx);
    EXPECT_EQ(carver.window().size(), 4u);
    EXPECT_EQ(carver.window_dates(), 2u);
}

// The cap counts DATES. With the old bar cap, `lookback_period` 252 on a 36-symbol book was
// seven sessions; the same number now means what it says.
TEST(CarverArm1WindowTest, TheCapCountsDatesNotBars) {
    RiskConfig c = tight_config();
    c.lookback_period = 5;  // five DATES
    CarverRiskModule carver("carver", c);
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;

    for (int d = 0; d < 8; ++d) {
        carver.begin_rebalance(start);
        RiskContext ctx = lap_ctx(1);
        carver.on_bars({make_bar("TSTA", d, 100.0 + d), make_bar("TSTB", d, 200.0 + d),
                        make_bar("TSTC", d, 50.0 + d)},
                       ctx);
    }
    EXPECT_EQ(carver.window_dates(), 5u) << "the newest five dates";
    EXPECT_EQ(carver.window().size(), 15u) << "3 symbols x 5 dates, not 5 bars";
    // The oldest surviving date is day 3, so day 2 and earlier are gone.
    for (const auto& bar : carver.window()) {
        EXPECT_GE(bar.timestamp, day(3));
    }
}

// F5 drops a date on which some symbol did not print, but only above the floor: below it the
// gate reads the unfiltered window rather than a 21-date matrix.
TEST(CarverArm1WindowTest, F5DoesNotEngageBelowTheFloor) {
    RiskConfig c = tight_config();
    c.lookback_period = 400;
    CarverRiskModule carver("carver", c);
    std::vector<Bar> w = dense_window(30, 3);
    w.push_back(make_bar("SYM0", 30, 130.0));  // a sparse date: only SYM0 printed
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    RiskContext ctx = lap_ctx(1);
    carver.on_bars(w, ctx);

    EXPECT_FALSE(carver.f5_engaged()) << "31 dates is below the 120 floor";
    EXPECT_EQ(carver.dates_dropped(), 0u);
    EXPECT_EQ(carver.window_dates(), 31u);
}

TEST(CarverArm1WindowTest, F5DropsSparseDatesAboveTheFloor) {
    RiskConfig c = tight_config();
    c.lookback_period = 400;
    CarverRiskModule carver("carver", c);
    // 130 complete dates, then three sparse ones.
    std::vector<Bar> w = dense_window(130, 3);
    for (int d = 130; d < 133; ++d) w.push_back(make_bar("SYM0", d, 100.0 + d));
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    RiskContext ctx = lap_ctx(1);
    carver.on_bars(w, ctx);

    EXPECT_TRUE(carver.f5_engaged()) << "130 complete dates is at or above the floor of 120";
    EXPECT_EQ(carver.dates_dropped(), 3u);
    EXPECT_EQ(carver.window_dates(), 133u) << "the window keeps them; the GATE does not see them";
    EXPECT_EQ(CarverRiskModule::kF5MinGateDates, 120u);
}

// THE level cut. The gate is scale-invariant, so the shipped loop charged the same ~0.85 on
// every lap and five laps shipped 0.4437 of the book. A level charges the head-room only.
namespace {

/// A config in which an INVARIANT term binds and leverage does not: a tight VaR limit against
/// leverage caps the book cannot reach. tight_config() binds on LEVERAGE, which is a per-lap
/// RATE by design, so it cannot test the level rule.
RiskConfig var_bound_config() {
    RiskConfig c = tight_config();
    c.var_limit = 0.10;            // VaR reads ~0.2006 on correlated_window(): binds
    c.jump_risk_limit = 10.0;      // never binds
    c.max_correlation = 0.999;     // never binds
    c.max_gross_leverage = 100.0;  // never binds
    c.max_net_leverage = 100.0;    // never binds
    return c;
}

}  // namespace

// THE level cut. The gate is scale-invariant, so the shipped loop charged the same ~0.85 on
// every lap and five laps shipped 0.4437 of a book the gate asked to cut by 15 %. With a level,
// lap 2 asks for the HEAD-ROOM only -- and when the composition has not changed, that is
// nothing at all.
TEST(CarverArm1LevelCutTest, AnInvariantRequestIsALevelNotARatePerLap) {
    CarverRiskModule carver("carver", var_bound_config());
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);

    const Book book = {{"TSTA", make_pos("TSTA", 10.0, 100.0)},
                       {"TSTC", make_pos("TSTC", 10.0, 50.0)}};
    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    auto d1 = carver.evaluate(book, lap1);
    ASSERT_TRUE(d1.is_ok());
    ASSERT_EQ(d1.value().action, RiskAction::SCALE)
        << "the VaR limit must bind for this test to mean anything";
    const double first = d1.value().scale;
    EXPECT_LT(first, 1.0);
    // The binding term is an INVARIANT one, not leverage: that is what makes it a level.
    EXPECT_DOUBLE_EQ(carver.last_invariant(), first);
    EXPECT_DOUBLE_EQ(carver.last_leverage(), 1.0);

    // The PM applies it and reports the QUANTISED factor actually multiplied in.
    RiskApplied applied;
    applied.action = RiskAction::SCALE;
    applied.requested_scale = first;
    applied.factor = Decimal(first);
    applied.won = true;
    carver.on_applied(applied, lap1);
    EXPECT_DOUBLE_EQ(carver.applied_level(), static_cast<double>(Decimal(first)));

    // Lap 2 reads the SAME invariant term off a book whose composition has not changed (the
    // gate normalises by the book's own gross, so shrinking it does not move the reading).
    // The old rule asked for `first` again and compounded it; the level rule asks for nothing.
    RiskContext lap2 = lap_ctx(2);
    carver.on_bars(correlated_window(), lap2);
    auto d2 = carver.evaluate(book, lap2);
    ASSERT_TRUE(d2.is_ok());
    EXPECT_DOUBLE_EQ(carver.last_invariant(), first) << "the reading is unchanged";
    EXPECT_EQ(d2.value().action, RiskAction::NONE)
        << "already cut to this level; the old rule would have asked for " << first << " again";
    EXPECT_DOUBLE_EQ(d2.value().scale, 1.0);
}

// T-6b-fix F1. The PM multiplies the book by Decimal(factor), which rounds to 8 decimals, and the
// module's level advances by that QUANTISED factor. When lap 1's factor rounds UP, lap 2 reads the
// same invariant term off an unchanged composition and its head-room is s_inv / Decimal(s_inv) =
// 0.99999999x: before the fix the module asked for that as a SCALE, the PM applied Decimal(scale) =
// 0.99999999 to a whole-contract book, and 20 of the futures backtest's 380 executions were stored
// as 0.99999999 / 1.00000003 contracts. The value AnInvariantRequestIsALevelNotARatePerLap happens
// to use rounds DOWN, so its head-room is above 1 and it could never see this.
namespace {

/// A VaR-bound Carver module whose lap-1 factor Decimal() rounds in the requested direction.
/// portfolio_multiplier is min(1, var_limit / sigma), so the limit steers the factor; the search
/// walks the limit in 1e-12 steps of sigma until the factor rounds the asked way by at least
/// 1e-10, so the lap-2 head-room is unambiguously on that side of 1.
struct SteeredLevel {
    RiskConfig config;
    double first = 1.0;
    bool found = false;
};

SteeredLevel steer_level(const std::vector<Bar>& window,
                         const std::unordered_map<std::string, Position>& book, bool round_up) {
    SteeredLevel out;
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    // sigma from a probe at the default VaR limit: multiplier = limit / sigma.
    CarverRiskModule probe("probe", var_bound_config());
    probe.begin_rebalance(start);
    RiskContext lap1 = lap_ctx(1);
    probe.on_bars(window, lap1);
    auto p = probe.evaluate(book, lap1);
    if (!p.is_ok() || p.value().action != RiskAction::SCALE) return out;
    const double sigma = var_bound_config().var_limit / p.value().scale;
    for (int k = 0; k < 20000 && !out.found; ++k) {
        RiskConfig c = var_bound_config();
        c.var_limit = sigma * (0.87458705524211799 + 1e-12 * k);
        CarverRiskModule m("carver", c);
        m.begin_rebalance(start);
        RiskContext l1 = lap_ctx(1);
        m.on_bars(window, l1);
        auto d = m.evaluate(book, l1);
        if (!d.is_ok() || d.value().action != RiskAction::SCALE) continue;
        const double f = d.value().scale;
        const double q = static_cast<double>(Decimal(f));
        if ((round_up && q - f > 1e-10) || (!round_up && f - q > 1e-10)) {
            out.config = c;
            out.first = f;
            out.found = true;
        }
    }
    return out;
}

}  // namespace

TEST(CarverArm1LevelCutTest, ALevelWhoseFactorRoundedUpIsNotChargedAgain) {
    const Book book = {{"TSTA", make_pos("TSTA", 10.0, 100.0)},
                       {"TSTC", make_pos("TSTC", 10.0, 50.0)}};
    const SteeredLevel s = steer_level(correlated_window(), book, /*round_up=*/true);
    ASSERT_TRUE(s.found) << "no VaR limit gave a lap-1 factor that rounds UP at 8 decimals";
    const double q = static_cast<double>(Decimal(s.first));
    ASSERT_GT(q, s.first) << "the precondition of this test: Decimal() rounded the factor UP";
    ASSERT_LT(s.first / q, 1.0) << "so the lap-2 head-room of an unchanged reading is below 1";

    CarverRiskModule carver("carver", s.config);
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    auto d1 = carver.evaluate(book, lap1);
    ASSERT_TRUE(d1.is_ok());
    ASSERT_EQ(d1.value().action, RiskAction::SCALE);
    ASSERT_EQ(d1.value().scale, s.first);

    // The PM multiplies by Decimal(first) and reports that quantised factor back.
    RiskApplied applied;
    applied.action = RiskAction::SCALE;
    applied.requested_scale = s.first;
    applied.factor = Decimal(s.first);
    applied.won = true;
    carver.on_applied(applied, lap1);
    ASSERT_EQ(carver.applied_level(), q) << "the level is the quantised factor (the contract)";

    // Lap 2: same window, same composition, so the same invariant reading.
    RiskContext lap2 = lap_ctx(2);
    carver.on_bars(correlated_window(), lap2);
    auto d2 = carver.evaluate(book, lap2);
    ASSERT_TRUE(d2.is_ok());
    ASSERT_EQ(carver.last_invariant(), s.first) << "the reading did not move";
    EXPECT_EQ(d2.value().action, RiskAction::NONE)
        << "a head-room of " << s.first / q << " is the level's own rounding, not a request; "
        << "it was returned as SCALE " << d2.value().scale;
    EXPECT_EQ(d2.value().scale, 1.0);

    // What that SCALE did to a whole contract: the PM's `pos.quantity *= scale`.
    Decimal one_lot(1.0);
    if (d2.value().action == RiskAction::SCALE) one_lot *= d2.value().scale;
    EXPECT_EQ(static_cast<double>(one_lot), 1.0) << "a 1-lot must still be exactly 1 contract";
}

// The mirror: a factor that rounds DOWN leaves a head-room above 1, which was already NONE.
TEST(CarverArm1LevelCutTest, ALevelWhoseFactorRoundedDownIsNotChargedAgain) {
    const Book book = {{"TSTA", make_pos("TSTA", 10.0, 100.0)},
                       {"TSTC", make_pos("TSTC", 10.0, 50.0)}};
    const SteeredLevel s = steer_level(correlated_window(), book, /*round_up=*/false);
    ASSERT_TRUE(s.found);
    const double q = static_cast<double>(Decimal(s.first));
    ASSERT_LT(q, s.first);

    CarverRiskModule carver("carver", s.config);
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    ASSERT_TRUE(carver.evaluate(book, lap1).is_ok());
    RiskApplied applied;
    applied.action = RiskAction::SCALE;
    applied.requested_scale = s.first;
    applied.factor = Decimal(s.first);
    applied.won = true;
    carver.on_applied(applied, lap1);

    RiskContext lap2 = lap_ctx(2);
    carver.on_bars(correlated_window(), lap2);
    auto d2 = carver.evaluate(book, lap2);
    ASSERT_TRUE(d2.is_ok());
    EXPECT_EQ(d2.value().action, RiskAction::NONE);
    EXPECT_EQ(d2.value().scale, 1.0);
}

// The guard must not swallow a lap that GENUINELY asks for a deeper cut: lap 2 reads a book of a
// different composition whose invariant term is well below the level already applied, and is
// charged exactly the head-room s_inv / level, as before.
TEST(CarverArm1LevelCutTest, AGenuinelyDeeperInvariantRequestIsStillCharged) {
    const Book book1 = {{"TSTA", make_pos("TSTA", 10.0, 100.0)},
                        {"TSTC", make_pos("TSTC", 10.0, 50.0)}};
    const SteeredLevel s = steer_level(correlated_window(), book1, /*round_up=*/true);
    ASSERT_TRUE(s.found);

    CarverRiskModule carver("carver", s.config);
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    ASSERT_TRUE(carver.evaluate(book1, lap1).is_ok());
    RiskApplied applied;
    applied.action = RiskAction::SCALE;
    applied.requested_scale = s.first;
    applied.factor = Decimal(s.first);
    applied.won = true;
    carver.on_applied(applied, lap1);
    const double level = carver.applied_level();

    // TSTC alone: the more volatile name with no diversification, so more VaR per unit of gross.
    const Book book2 = {{"TSTC", make_pos("TSTC", 10.0, 50.0)}};
    RiskContext lap2 = lap_ctx(2);
    carver.on_bars(correlated_window(), lap2);
    auto d2 = carver.evaluate(book2, lap2);
    ASSERT_TRUE(d2.is_ok());
    const double s_inv2 = carver.last_invariant();
    ASSERT_LT(s_inv2, level - 1e-4) << "the precondition: lap 2 genuinely asks for a deeper level";
    ASSERT_EQ(carver.last_leverage(), 1.0);
    EXPECT_EQ(d2.value().action, RiskAction::SCALE);
    EXPECT_EQ(d2.value().scale, s_inv2 / level) << "charged exactly the head-room, bit for bit";
}

// The leverage term is NOT a level: it is a magnitude read off the book as it now stands, so it
// is charged again on every lap. tight_config() binds on leverage, which is why it cannot be
// used for the test above.
TEST(CarverArm1LevelCutTest, TheLeverageTermStaysAPerLapRate) {
    CarverRiskModule carver("carver", tight_config());
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    const Book book = {{"TSTA", make_pos("TSTA", 1000.0, 100.0)}};

    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    auto d1 = carver.evaluate(book, lap1);
    ASSERT_TRUE(d1.is_ok());
    ASSERT_EQ(d1.value().action, RiskAction::SCALE);
    EXPECT_DOUBLE_EQ(carver.last_invariant(), 1.0) << "no invariant term binds here";
    EXPECT_LT(carver.last_leverage(), 1.0);
    EXPECT_DOUBLE_EQ(d1.value().scale, carver.last_leverage());

    RiskApplied applied;
    applied.action = RiskAction::SCALE;
    applied.factor = Decimal(d1.value().scale);
    applied.won = true;
    carver.on_applied(applied, lap1);

    // The same book is handed back (the PM would have shrunk it; here it has not), so leverage
    // reads the same and is charged again. That is the rate semantics, deliberately kept.
    RiskContext lap2 = lap_ctx(2);
    carver.on_bars(correlated_window(), lap2);
    auto d2 = carver.evaluate(book, lap2);
    ASSERT_TRUE(d2.is_ok());
    EXPECT_EQ(d2.value().action, RiskAction::SCALE);
    EXPECT_DOUBLE_EQ(d2.value().scale, carver.last_leverage());
}

// A level marked PARTIAL is not a true statement about the book the module measured, so nothing
// divides by it: that lap honours the leverage RATE alone. (Zero laps on every shipped book.)
TEST(CarverArm1LevelCutTest, APartialLevelIsNotDividedBy) {
    CarverRiskModule carver("carver", tight_config());
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    const Book book = {{"TSTA", make_pos("TSTA", 1000.0, 100.0)}};

    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    auto d1 = carver.evaluate(book, lap1);
    ASSERT_TRUE(d1.is_ok());
    ASSERT_EQ(d1.value().action, RiskAction::SCALE);

    RiskApplied partial;
    partial.action = RiskAction::SCALE;
    partial.factor = Decimal(d1.value().scale);
    partial.won = true;
    partial.partial = true;  // a pinned sleeve was skipped by the multiply
    partial.scopes_skipped = 1;
    carver.on_applied(partial, lap1);
    EXPECT_TRUE(carver.level_partial());

    RiskContext lap2 = lap_ctx(2);
    carver.on_bars(correlated_window(), lap2);
    auto d2 = carver.evaluate(book, lap2);
    ASSERT_TRUE(d2.is_ok());
    // The leverage term is the only one that may still bind, and it is read fresh each lap.
    EXPECT_DOUBLE_EQ(d2.value().scale, std::min(1.0, carver.last_leverage()));
}

// begin_rebalance clears the partial mark with the rest of the per-rebalance state.
TEST(CarverArm1LevelCutTest, BeginRebalanceClearsThePartialMark) {
    CarverRiskModule carver("carver", tight_config());
    RiskContext lap1 = lap_ctx(1);
    RiskApplied partial;
    partial.action = RiskAction::SCALE;
    partial.factor = Decimal(0.5);
    partial.partial = true;
    carver.on_applied(partial, lap1);
    ASSERT_TRUE(carver.level_partial());
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    EXPECT_FALSE(carver.level_partial());
    EXPECT_EQ(carver.applied_level(), 1.0);
}

// ===== T-6b commit 9b: the window's MarketData is built once per rebalance =====

// After commit 9 the window changes only on the APPENDING lap, so F5's scan and
// create_market_data produced an identical MarketData on laps 2..n. This pins that the result is
// the same object's contents either way -- the commit is a pure removal of repeated work, and if
// it ever stopped being one this test says so.
TEST(CarverArm1WindowTest, TheMarketDataIsIdenticalOnEveryLapOfARebalance) {
    RiskConfig c = tight_config();
    c.lookback_period = 400;
    CarverRiskModule carver("carver", c);
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;
    carver.begin_rebalance(start);
    const std::vector<Bar> w = correlated_window();

    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(w, lap1);
    const Book book = {{"TSTA", make_pos("TSTA", 10.0, 100.0)}};
    auto first = carver.evaluate(book, lap1);
    ASSERT_TRUE(first.is_ok());
    const size_t dates1 = carver.window_dates();
    const size_t dropped1 = carver.dates_dropped();
    const bool f5_1 = carver.f5_engaged();
    const std::vector<std::vector<double>> returns1 = carver.market_data().returns;
    const std::vector<std::vector<double>> cov1 = carver.market_data().covariance;

    // Laps 2..5 hand on_bars the SAME bars, as the loop does.
    for (int lap = 2; lap <= 5; ++lap) {
        RiskContext ctx = lap_ctx(lap);
        carver.on_bars(w, ctx);
        EXPECT_EQ(carver.window_dates(), dates1) << "lap " << lap;
        EXPECT_EQ(carver.dates_dropped(), dropped1) << "lap " << lap;
        EXPECT_EQ(carver.f5_engaged(), f5_1) << "lap " << lap;
        // Bit for bit, not merely the same shape.
        EXPECT_EQ(carver.market_data().returns, returns1) << "lap " << lap;
        EXPECT_EQ(carver.market_data().covariance, cov1) << "lap " << lap;
        auto again = carver.evaluate(book, ctx);
        ASSERT_TRUE(again.is_ok()) << "lap " << lap;
        // The same book against the same window gives the same reading, bit for bit.
        EXPECT_DOUBLE_EQ(carver.last_invariant(), first.value().metrics
                             ? std::min({static_cast<double>(first.value().metrics->portfolio_multiplier),
                                         static_cast<double>(first.value().metrics->jump_multiplier),
                                         static_cast<double>(first.value().metrics->correlation_multiplier)})
                             : 1.0)
            << "lap " << lap;
    }
    // The window itself never grew: five laps, one append.
    EXPECT_EQ(carver.window().size(), w.size());
}

// A NEW rebalance rebuilds it, so the cache cannot outlive the window it describes.
TEST(CarverArm1WindowTest, ANewRebalanceRebuildsTheMarketData) {
    RiskConfig c = tight_config();
    c.lookback_period = 400;
    CarverRiskModule carver("carver", c);
    RiskContext start;
    start.phase = RiskPhase::REBALANCE_START;

    carver.begin_rebalance(start);
    RiskContext lap1 = lap_ctx(1);
    carver.on_bars(correlated_window(), lap1);
    const size_t dates_after_first = carver.window_dates();
    const size_t returns_after_first = carver.market_data().returns.size();

    // A second rebalance with a new date must be SEEN by the gate, not masked by the cache.
    carver.begin_rebalance(start);
    RiskContext lap1b = lap_ctx(1);
    carver.on_bars({make_bar("TSTA", 30, 111.0), make_bar("TSTB", 30, 222.0),
                    make_bar("TSTC", 30, 55.0)},
                   lap1b);
    EXPECT_EQ(carver.window_dates(), dates_after_first + 1) << "the new date reached the window";
    // ...and the MarketData was REBUILT from it. This is the assertion that makes the test
    // load-bearing: window_dates() reads the window directly, so it would still be right if the
    // cache had gone stale. returns.size() comes from market_data_, so it is only right if
    // begin_rebalance cleared the built flag.
    EXPECT_EQ(carver.market_data().returns.size(), returns_after_first + 1)
        << "the cached MarketData outlived the rebalance it was built for";
}
