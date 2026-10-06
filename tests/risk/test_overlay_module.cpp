// LOOP_SPEC v6.2 section 4, inside today's lap loop: with the overlay's limits set and the
// rebalance's inputs on the context, the carver module reads the book in capital terms. Its request
// on a lap is m, the smallest of the five multipliers on that lap's book: a rate, never a level.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/risk/carver_risk_module.hpp"
#include "trade_ngin/risk/overlay.hpp"

using namespace trade_ngin;

namespace {

// Two participants with 260 dates of returns each, weakly correlated; AAA about 16% a year and BBB
// about 32%.
overlay::Inputs inputs(double tau = 0.20) {
    std::vector<double> day, a, b;
    unsigned state = 2463534242u;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state % 100000u) / 100000.0 - 0.5;
    };
    double d = 19000.0;
    for (int k = 0; k < 260; ++k) {
        d += k % 5 == 4 ? 3.0 : 1.0;
        day.push_back(d);
        const double common = next();
        a.push_back(0.012 * common + 0.03 * next());
        b.push_back(0.012 * common + 0.06 * next());
    }
    std::vector<overlay::ParticipantSeries> series(2);
    for (int i = 0; i < 2; ++i) {
        series[i].present = true;
        series[i].day = &day;
        series[i].returns = i == 0 ? &a : &b;
        series[i].close = i == 0 ? 100.0 : 50.0;
        series[i].multiplier = i == 0 ? 10.0 : 100.0;
        series[i].jump_sigma_daily = i == 0 ? 0.015 : 0.03;
    }
    return overlay::build_inputs(tau, {"AAA", "BBB"}, series);
}

Position position(const std::string& symbol, double quantity) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(quantity);
    p.average_price = Price(1.0);
    return p;
}

CarverRiskModule module(bool with_limits = true) {
    RiskConfig rc;
    rc.var_limit = 0.25;
    rc.jump_risk_limit = 0.05;
    rc.max_correlation = 0.85;
    rc.max_gross_leverage = 8.0;
    rc.max_net_leverage = 6.0;
    rc.confidence_level = 0.99;
    rc.lookback_period = 252;
    rc.capital = Decimal(500'000.0);
    CarverRiskModule m("carver", rc, 21);
    if (with_limits) m.set_overlay_limits({2.25, 4.5, 4.0, 8.0, 6.0});
    return m;
}

RiskContext context(const overlay::Inputs* in, int lap = 1) {
    RiskContext ctx;
    ctx.phase = RiskPhase::LAP;
    ctx.lap = lap;
    ctx.portfolio_id = "OVERLAY_TEST";
    ctx.overlay_inputs = in;
    return ctx;
}

}  // namespace

// A small book: every reading under its limit, no request. The readings are the overlay's on the
// weights x = N M P / E.
TEST(OverlayModule, ASmallBookAsksForNothing) {
    const auto in = inputs();
    auto m = module();
    const std::unordered_map<std::string, Position> book = {{"AAA", position("AAA", 100.0)},
                                                            {"BBB", position("BBB", -20.0)}};
    auto d = m.evaluate(book, context(&in));
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value().action, RiskAction::NONE);
    ASSERT_TRUE(m.overlay_read());
    const auto& e = m.last_overlay();
    EXPECT_NEAR(e.weights[0], 100.0 * 10.0 * 100.0 / 500'000.0, 1e-12);  // 0.2
    EXPECT_NEAR(e.weights[1], -20.0 * 100.0 * 50.0 / 500'000.0, 1e-12);  // -0.2
    EXPECT_NEAR(e.readings.gross, 0.4, 1e-12);
    EXPECT_NEAR(e.readings.net, 0.0, 1e-12);
    EXPECT_DOUBLE_EQ(e.limits.risk, 0.45);
    EXPECT_DOUBLE_EQ(e.limits.jump, 0.90);
    EXPECT_DOUBLE_EQ(e.limits.shock, 0.80);
    EXPECT_DOUBLE_EQ(e.multiplier.m, 1.0);
    EXPECT_EQ(e.multiplier.binding, "none");
    EXPECT_FALSE(d.value().blind);
}

// A book thirty times larger: the module asks for m, and a book cut by m reads the binding term
// exactly at its limit and asks for nothing more. On the next lap the request is again the fresh
// reading of that lap's book: a level already applied does not hide a new excess.
TEST(OverlayModule, TheRequestIsTheSmallestMultiplierAndItIsARate) {
    const auto in = inputs();
    auto m = module();
    std::unordered_map<std::string, Position> book = {{"AAA", position("AAA", 3000.0)},
                                                      {"BBB", position("BBB", 600.0)}};
    auto d = m.evaluate(book, context(&in, 1));
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(d.value().action, RiskAction::SCALE);
    const overlay::Evaluation first = m.last_overlay();
    EXPECT_LT(first.multiplier.m, 1.0);
    EXPECT_DOUBLE_EQ(d.value().scale, first.multiplier.m);
    EXPECT_NE(first.multiplier.binding, "none");
    ASSERT_TRUE(d.value().metrics.has_value());
    EXPECT_DOUBLE_EQ(d.value().metrics->recommended_scale, first.multiplier.m);
    EXPECT_DOUBLE_EQ(d.value().metrics->portfolio_var, first.readings.risk);
    EXPECT_DOUBLE_EQ(d.value().metrics->gross_leverage, first.readings.gross);

    // The PM multiplies the book by the factor and tells the module.
    RiskApplied applied;
    applied.action = RiskAction::SCALE;
    applied.factor = Decimal(first.multiplier.m);
    m.on_applied(applied, context(&in, 1));
    for (auto& [symbol, p] : book) p.quantity = Quantity(static_cast<double>(p.quantity) * first.multiplier.m);
    d = m.evaluate(book, context(&in, 2));
    ASSERT_TRUE(d.is_ok());
    EXPECT_EQ(d.value().action, RiskAction::NONE) << "the cut book sits at the limit that asked";
    EXPECT_NEAR(m.last_overlay().multiplier.m, 1.0, 1e-7);

    // A lap whose book is over again (rounding put contracts back) is asked for its own multiplier,
    // not for that multiplier divided by the level already applied.
    for (auto& [symbol, p] : book) p.quantity = Quantity(static_cast<double>(p.quantity) * 1.10);
    d = m.evaluate(book, context(&in, 3));
    ASSERT_TRUE(d.is_ok());
    ASSERT_EQ(d.value().action, RiskAction::SCALE);
    EXPECT_NEAR(d.value().scale, 1.0 / 1.10, 1e-7);
    EXPECT_DOUBLE_EQ(d.value().scale, m.last_overlay().multiplier.m);
}

// The three risk limits are ratios to tau: a higher tau raises them and the same book asks for less.
TEST(OverlayModule, TheRiskLimitsAreRatiosToTau) {
    const auto low = inputs(0.20);
    const auto high = inputs(0.40);
    const std::unordered_map<std::string, Position> book = {{"AAA", position("AAA", 2000.0)},
                                                            {"BBB", position("BBB", 400.0)}};
    auto a = module();
    auto b = module();
    ASSERT_TRUE(a.evaluate(book, context(&low)).is_ok());
    ASSERT_TRUE(b.evaluate(book, context(&high)).is_ok());
    EXPECT_DOUBLE_EQ(b.last_overlay().limits.risk, 2.0 * a.last_overlay().limits.risk);
    EXPECT_DOUBLE_EQ(b.last_overlay().limits.gross, a.last_overlay().limits.gross);
    EXPECT_GT(b.last_overlay().multiplier.risk, a.last_overlay().multiplier.risk);
}

// Fewer than 21 complete dates: BLIND. The covariance terms ask for nothing; leverage still does.
TEST(OverlayModule, ABlindWindowStillEnforcesLeverage) {
    auto in = inputs();
    in.returns.erase(in.returns.begin(), in.returns.end() - 15);
    in.ordinals.erase(in.ordinals.begin(), in.ordinals.end() - 15);
    auto m = module();
    const std::unordered_map<std::string, Position> book = {{"AAA", position("AAA", 5000.0)}};
    auto d = m.evaluate(book, context(&in));
    ASSERT_TRUE(d.is_ok());
    EXPECT_TRUE(d.value().blind);
    EXPECT_FALSE(m.last_overlay().readings.covariance_readings);
    EXPECT_NEAR(m.last_overlay().readings.gross, 10.0, 1e-12);
    ASSERT_EQ(d.value().action, RiskAction::SCALE);
    EXPECT_NEAR(d.value().scale, 0.6, 1e-12) << "net leverage 10 against 6";
    EXPECT_EQ(m.last_overlay().multiplier.binding, "L_n");
}

// Control: without the limits, or on a context with no inputs, the module does not read the book in
// capital terms.
TEST(OverlayModule, WithoutLimitsOrInputsTheModuleReadsTheBookAsBefore) {
    const auto in = inputs();
    const std::unordered_map<std::string, Position> book = {{"AAA", position("AAA", 3000.0)}};
    auto no_limits = module(false);
    ASSERT_TRUE(no_limits.evaluate(book, context(&in)).is_ok());
    EXPECT_FALSE(no_limits.overlay_read());
    auto no_inputs = module(true);
    ASSERT_TRUE(no_inputs.evaluate(book, context(nullptr)).is_ok());
    EXPECT_FALSE(no_inputs.overlay_read());
}
