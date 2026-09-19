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
    EXPECT_EQ(carver.capabilities(), std::set<RiskAction>{RiskAction::SCALE});
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
