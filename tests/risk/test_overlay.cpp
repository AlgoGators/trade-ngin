// The risk overlay's arithmetic in capital terms (LOOP_SPEC v6.2 section 4): the gate window, the
// four readings and the multiplier, against values the frozen reference implementation
// (docs/after_equities/stage3/oracle/book.py: gate, readings, multiplier) gives on the same
// returns. The returns come from one deterministic generator written the same way on both sides.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "trade_ngin/risk/overlay.hpp"

using namespace trade_ngin::overlay;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kDates = 300;
constexpr int kParticipants = 6;

std::vector<std::vector<double>> generated() {
    std::vector<std::vector<double>> r(kDates, std::vector<double>(kParticipants, 0.0));
    unsigned state = 2463534242u;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state % 100000u) / 100000.0 - 0.5;
    };
    for (int d = 0; d < kDates; ++d) {
        const double common = next();
        for (int i = 0; i < kParticipants; ++i) {
            const double own = next();
            r[d][i] = 0.012 * common + 0.01 * (1.0 + 0.3 * i) * own;
        }
    }
    return r;
}

std::vector<double> ordinals() {
    std::vector<double> o;
    double d = 730000.0;
    for (int k = 0; k < kDates; ++k) {
        d += k % 5 == 4 ? 3.0 : 1.0;
        o.push_back(d);
    }
    return o;
}

// Fixture A: participant 1 starts 40 dates late, participant 3 has 60 returns (a short history),
// participant 5 has none, and two participants have a hole.
std::vector<std::vector<double>> fixture_a() {
    auto r = generated();
    for (int d = 0; d < 40; ++d) r[d][1] = kNaN;
    for (int d = 0; d < kDates - 60; ++d) r[d][3] = kNaN;
    for (int d = 0; d < kDates; ++d) r[d][5] = kNaN;
    for (int d = 10; d < 14; ++d) r[d][0] = kNaN;
    for (int d = 200; d < 203; ++d) r[d][2] = kNaN;
    return r;
}

const std::vector<double> kWeights = {0.8, -0.5, 1.2, 0.3, -0.9, 0.4};
const std::vector<double> kJump = {0.25, 0.40, 0.18, 0.55, 0.30, 0.22};
const Limits kLimits = {0.45, 0.90, 0.80, 8.0, 6.0};

void expect_rel(double got, double want) { EXPECT_NEAR(got, want, 1e-12 * std::abs(want)); }

}  // namespace

TEST(Overlay, TheGateWindowOnCompleteDates) {
    const auto g = gate_window(fixture_a(), ordinals());
    EXPECT_EQ(g.mode, GateWindow::Mode::kComplete);
    EXPECT_EQ(g.window_dates, 252u);
    EXPECT_EQ(g.complete_dates, 249u);
    expect_rel(g.bars_per_year, 256.6062322946176);
    EXPECT_EQ(g.in_r, (std::vector<char>{1, 1, 1, 0, 1, 0}));
    EXPECT_EQ(g.in_shock, (std::vector<char>{1, 1, 1, 1, 1, 0}));
    EXPECT_EQ(g.no_return, (std::vector<char>{0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(g.short_history, (std::vector<char>{0, 0, 0, 1, 0, 0}));
    const double shock[] = {0.07148332110831575, 0.08212901211690823, 0.08970021123152437,
                            0.11147914049258474, 0.11435405086915204};
    for (int i = 0; i < 5; ++i) expect_rel(g.shock_sigma[i], shock[i]);
    ASSERT_EQ(g.covariance.size(), 4u) << "the covariance is over the participants in R";
}

TEST(Overlay, TheFourReadingsAndNoCut) {
    const auto g = gate_window(fixture_a(), ordinals());
    const auto rd = readings(kWeights, g, kJump);
    ASSERT_TRUE(rd.covariance_readings);
    expect_rel(rd.risk, 0.13961744160414663);
    expect_rel(rd.jump, 0.3657611625283111);
    expect_rel(rd.shock, 0.3422538043529482);
    expect_rel(rd.gross, 4.1);
    expect_rel(rd.net, 1.3);
    const auto m = multiplier(rd, kLimits);
    EXPECT_DOUBLE_EQ(m.m, 1.0);
    EXPECT_EQ(m.binding, "none");
}

// Four times the book: every covariance reading and the gross leverage are over; the smallest
// multiplier is the gross leverage's, and it binds.
TEST(Overlay, TheSmallestMultiplierBindsAndEveryReadingScalesWithTheBook) {
    const auto g = gate_window(fixture_a(), ordinals());
    std::vector<double> big = kWeights;
    for (double& x : big) x *= 4.0;
    const auto rd = readings(big, g, kJump);
    expect_rel(rd.risk, 0.5584697664165865);
    expect_rel(rd.jump, 1.4630446501132444);
    expect_rel(rd.shock, 1.3690152174117929);
    expect_rel(rd.gross, 16.4);
    expect_rel(rd.net, 5.2);
    const auto m = multiplier(rd, kLimits);
    expect_rel(m.m, 0.48780487804878053);
    EXPECT_EQ(m.binding, "L_g");
    expect_rel(m.risk, 0.8057732523058835);
    expect_rel(m.jump, 0.6151555251101442);
    expect_rel(m.shock, 0.5843616563389624);
    expect_rel(m.gross, 0.48780487804878053);
    EXPECT_DOUBLE_EQ(m.net, 1.0);
    // A cut of m brings the term that asked exactly to its limit.
    std::vector<double> cut = big;
    for (double& x : cut) x *= m.m;
    expect_rel(readings(cut, g, kJump).gross, kLimits.gross);
}

// A long book: the jump term asks for the deepest cut and binds; the net term reads |L_net|.
TEST(Overlay, TheJumpTermBindsOnALongBook) {
    const auto g = gate_window(fixture_a(), ordinals());
    const auto rd = readings({3.0, 2.5, 1.2, 0.3, 0.9, 0.4}, g, kJump);
    expect_rel(rd.risk, 0.4802628069165154);
    expect_rel(rd.jump, 1.7607391877324279);
    expect_rel(rd.shock, 0.6637751350250595);
    const auto m = multiplier(rd, kLimits);
    expect_rel(m.m, 0.5111489573643596);
    EXPECT_EQ(m.binding, "R_jump");
    expect_rel(m.risk, 0.9369869861236704);
    EXPECT_DOUBLE_EQ(m.shock, 1.0);
    expect_rel(m.gross, 0.9638554216867469);
    expect_rel(m.net, 0.7228915662650601);
    // a short book of the same size reads the same net multiplier
    Readings neg = rd;
    neg.net = -rd.net;
    expect_rel(multiplier(neg, kLimits).net, 0.7228915662650601);
}

// Fewer than 120 complete dates: the covariance is over every window date with the missing returns
// zero-filled, and the day is marked.
TEST(Overlay, BelowTheCompleteDateFloorTheMissingReturnsAreZeroFilled) {
    auto r = generated();
    for (int d = 0; d < kDates; ++d) {
        r[d][5] = kNaN;
        r[d][3] = kNaN;
    }
    for (int d = kDates - 252; d < kDates; ++d) {
        if (d % 2 == 0) r[d][0] = kNaN;
        if (d % 3 == 0) r[d][1] = kNaN;
    }
    const auto g = gate_window(r, ordinals());
    EXPECT_EQ(g.mode, GateWindow::Mode::kZeroFill);
    EXPECT_EQ(g.window_dates, 252u);
    EXPECT_EQ(g.complete_dates, 84u);
    expect_rel(g.bars_per_year, 86.61642857142857);
    EXPECT_EQ(g.in_r, (std::vector<char>{1, 1, 1, 0, 1, 0}));
    EXPECT_EQ(g.in_shock, (std::vector<char>{1, 1, 1, 0, 1, 0}));
    EXPECT_EQ(g.no_return, (std::vector<char>{0, 0, 0, 1, 0, 1}));
    const auto rd = readings(kWeights, g, kJump);
    expect_rel(rd.risk, 0.078644111203125);
    expect_rel(rd.jump, 0.39195093598651254);
    expect_rel(rd.shock, 0.16431198947205697);
}

// Fewer than 21 complete dates: BLIND. No covariance reading is computed and none asks for a cut;
// the two leverage readings still do.
TEST(Overlay, ABlindWindowKeepsTheLeverageTerms) {
    auto r = generated();
    for (int d = 0; d < kDates; ++d) {
        r[d][5] = kNaN;
        r[d][3] = kNaN;
    }
    for (int d = kDates - 252; d < kDates; ++d) {
        if (d % 2 == 0) r[d][0] = kNaN;
        if (d % 2 == 1 && d < kDates - 10) r[d][1] = kNaN;
    }
    const auto g = gate_window(r, ordinals());
    EXPECT_TRUE(g.blind());
    EXPECT_EQ(g.complete_dates, 5u);
    EXPECT_EQ(g.in_r, (std::vector<char>{1, 1, 1, 0, 1, 0}));
    std::vector<double> big = kWeights;
    for (double& x : big) x *= 4.0;
    const auto rd = readings(big, g, kJump);
    EXPECT_FALSE(rd.covariance_readings);
    expect_rel(rd.gross, 16.4);
    const auto m = multiplier(rd, kLimits);
    EXPECT_DOUBLE_EQ(m.risk, 1.0);
    EXPECT_DOUBLE_EQ(m.jump, 1.0);
    EXPECT_DOUBLE_EQ(m.shock, 1.0);
    expect_rel(m.m, 8.0 / 16.4);
    EXPECT_EQ(m.binding, "L_g");
}

// No participant with 120 returns: nothing is in R, the window is blind.
TEST(Overlay, AWindowWithNoLongHistoryIsBlind) {
    auto r = generated();
    for (int d = 0; d < kDates - 100; ++d) {
        for (int i = 0; i < kParticipants; ++i) r[d][i] = kNaN;
    }
    const auto g = gate_window(r, ordinals());
    EXPECT_TRUE(g.blind());
    EXPECT_EQ(g.window_dates, 100u);
    EXPECT_EQ(g.in_r, (std::vector<char>{0, 0, 0, 0, 0, 0}));
    EXPECT_EQ(g.short_history, (std::vector<char>{1, 1, 1, 1, 1, 1}));
    EXPECT_TRUE(gate_window({}, {}).blind());
}

TEST(Overlay, ThePercentileInterpolatesLinearly) {
    EXPECT_DOUBLE_EQ(percentile({1.0, 2.0, 3.0, 4.0, 5.0}, 50.0), 3.0);
    EXPECT_DOUBLE_EQ(percentile({5.0, 1.0, 4.0, 2.0, 3.0}, 99.0), 4.96);
    EXPECT_TRUE(std::isnan(percentile({}, 99.0)));
}
