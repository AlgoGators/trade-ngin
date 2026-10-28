// The one optimisation pass (LOOP_SPEC v6.2 sections 4 to 6) as pure functions: the cap, the
// optimiser's covariance, the search from the held book, the buffer and the rounding, the clip and
// the trim, against values the frozen reference implementation (docs/after_equities/stage3/oracle/
// book.py) gives on the same inputs. The closes come from one deterministic generator written the
// same way on both sides.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include "trade_ngin/optimization/one_pass.hpp"
#include "trade_ngin/optimization/one_pass_log.hpp"
#include "trade_ngin/optimization/one_pass_record.hpp"

using namespace trade_ngin::one_pass;
namespace overlay = trade_ngin::overlay;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kDates = 120;
constexpr int kSymbols = 5;

struct Fixture {
    Matrix closes, levels;
    Vector ordinals;
    Vector u;  // weight of one contract on 500,000
};

Fixture fixture() {
    Fixture f;
    f.closes.assign(kDates, Vector(kSymbols, 0.0));
    f.levels.assign(kDates, Vector(kSymbols, 0.0));
    unsigned state = 2463534242u;
    auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state % 100000u) / 100000.0 - 0.5;
    };
    double p[kSymbols] = {100.0, 50.0, 2000.0, 1.2, 75.0};
    double adj[kSymbols] = {0.0, 0.0, 0.0, 0.0, 0.0};
    for (int d = 0; d < kDates; ++d) {
        const double common = next();
        for (int i = 0; i < kSymbols; ++i) {
            p[i] *= 1.0 + 0.012 * common + 0.02 * (1 + 0.2 * i) * next();
            f.closes[d][i] = p[i];
            f.levels[d][i] = p[i] + adj[i];
        }
        if (d == 60) {  // a roll step on symbol 1: the level carries on, the raw close steps down
            adj[1] += 3.0;
            p[1] -= 3.0;
        }
    }
    double o = 730000.0;
    for (int k = 0; k < kDates; ++k) {
        o += k % 5 == 4 ? 3.0 : 1.0;
        f.ordinals.push_back(o);
    }
    const double multiplier[kSymbols] = {10.0, 100.0, 2.0, 100000.0, 50.0};
    for (int i = 0; i < kSymbols; ++i) f.u.push_back(multiplier[i] * f.closes[kDates - 1][i] / 500000.0);
    // symbol 3 starts 15 dates late; symbol 0 has a hole
    for (int d = 0; d < 15; ++d) f.closes[d][3] = f.levels[d][3] = kNaN;
    for (int d = 40; d < 43; ++d) f.closes[d][0] = f.levels[d][0] = kNaN;
    return f;
}

const Mask kStale = {0, 0, 0, 0, 1};
const Vector kTarget = {0.9, -0.6, 0.35, 1.4, 0.0};
const Vector kHeld = {3.0, -10.0, 0.0, 1.0, 2.0};
const Vector kCost = {2.5 / 500000.0, 3.0 / 500000.0, 1.5 / 500000.0, 6.0 / 500000.0, 2.0 / 500000.0};

void expect_rel(double got, double want) { EXPECT_NEAR(got, want, 1e-11 * std::abs(want)); }

}  // namespace

// The covariance over the intersected dates of the symbols that are not stale: the return over two
// intersected dates is the adjusted change over the raw earlier close, so the roll step is no
// return; the stale symbol sits on the 0.01 diagonal alone.
TEST(OnePass, TheOptimisersCovariance) {
    const Fixture f = fixture();
    const Covariance c = optimiser_covariance(f.closes, f.levels, f.ordinals, kStale);
    EXPECT_EQ(c.filled, (Mask{0, 0, 0, 0, 1}));
    expect_rel(c.bars_per_year, 252.67294520547944);
    const double want[4][4] = {
        {0.011718278990549981, 0.004354137381387234, 0.003908326103519491, 0.005820651304818624},
        {0.004354137381387234, 0.015254226477479024, 0.005698984357600932, 0.003175046805055004},
        {0.003908326103519491, 0.005698984357600932, 0.022113673396599903, 0.005110509425123167},
        {0.005820651304818624, 0.003175046805055004, 0.005110509425123167, 0.027151159498584126}};
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) expect_rel(c.matrix[i][j], want[i][j]);
        EXPECT_DOUBLE_EQ(c.matrix[i][4], 0.0);
    }
    EXPECT_DOUBLE_EQ(c.matrix[4][4], 0.01);
}

// A symbol with too few closes to leave twenty returns in the intersection is dropped to the 0.01
// diagonal, and the others are estimated without it.
TEST(OnePass, AShortSymbolIsDroppedToTheDiagonal) {
    Fixture f = fixture();
    for (int d = 0; d < 105; ++d) f.closes[d][2] = f.levels[d][2] = kNaN;
    const Covariance c = optimiser_covariance(f.closes, f.levels, f.ordinals, kStale);
    EXPECT_EQ(c.filled, (Mask{0, 0, 1, 0, 1}));
    expect_rel(c.bars_per_year, 252.67294520547944);
    expect_rel(c.matrix[0][0], 0.011718278990549981);
    EXPECT_DOUBLE_EQ(c.matrix[2][2], 0.01);
    EXPECT_DOUBLE_EQ(c.matrix[0][2], 0.0);
}

// The search from the held book at the cost multiplier 100, then the buffer and the rounding.
TEST(OnePass, TheSearchFromTheHeldBookThenTheBuffer) {
    const Fixture f = fixture();
    const Covariance c = optimiser_covariance(f.closes, f.levels, f.ordinals, kStale);
    const double u_want[] = {0.00211525732418869, 0.01076812223362205, 0.007668515126348104,
                             0.2714123821450545, 0.00784671643986464};
    for (int i = 0; i < kSymbols; ++i) expect_rel(f.u[i], u_want[i]);
    expect_rel(tracking_error(kHeld, kTarget, c.matrix, f.u, kHeld, kCost, 100.0), 0.24341219221372465);
    EXPECT_EQ(pass_cap(kTarget, kHeld, f.u, 100), 1045);
    const Search s = search(kTarget, c.matrix, f.u, kHeld, kCost, 100.0, 2.0);
    EXPECT_EQ(s.book, (Vector{3.0, -10.0, 20.0, 6.0, 2.0}));
    expect_rel(s.tracking_error, 0.11022177522212591);
    EXPECT_EQ(s.passes, 25);
    EXPECT_FALSE(s.pass_capped);

    const Mask eligible = book_eligible(s.book, kHeld);
    EXPECT_EQ(eligible, (Mask{1, 1, 1, 1, 1}));
    const auto [b, b_index] = b_sigma(f.u, c.matrix, eligible, 0.20);
    expect_rel(b, 0.044722270829816);
    EXPECT_EQ(b_index, 3);
    const Buffered buffered = buffer(s.book, kHeld, f.u, c.matrix, b);
    EXPECT_TRUE(buffered.traded);
    EXPECT_FALSE(buffered.returned_to_held);
    expect_rel(buffered.te_held, 0.2294548686514714);
    expect_rel(buffered.a, 0.8050933889847135);
    EXPECT_EQ(buffered.book, (Vector{3.0, -10.0, 16.0, 5.0, 2.0}));
    expect_rel(buffered.unrounded[2], 16.10186777969427);
    expect_rel(buffered.unrounded[3], 5.025466944923568);
}

// optimiser_covariance "carver" (AFTS gap list C-4): strategy 25's covariance is sigma_i sigma_j
// rho_ij with the given sigmas (the 32-day EWMA sd x 16) and the overlay's weekly correlations.
// Symbol 1 is twice symbol 0 and symbol 2 its negative on every day, so rho is exactly +1 and -1;
// symbol 3 has no sigma and sits on the 0.01 diagonal alone; there is no date intersection and no
// stale rule, and the annualisation is 16 (256 bars a year). Returns and dates of different
// lengths estimate nothing.
TEST(OnePass, StrategyTwentyFivesCovariance) {
    Vector o;
    for (int w = 0; w < 40; ++w) {
        for (int d = 0; d < 5; ++d) o.push_back(21004.0 + 7.0 * w + d);
    }
    Matrix r(o.size(), Vector(4, 0.0));
    unsigned state = 99u;
    for (std::size_t d = 0; d < o.size(); ++d) {
        state = state * 1103515245u + 12345u;
        const double x = static_cast<double>(state % 1000u) / 1000.0 - 0.5;
        r[d] = {0.01 * x, 0.02 * x, -0.01 * x, d % 3 == 0 ? kNaN : 0.005 * x};
    }
    const Covariance c = strategy25_covariance(r, o, {0.20, 0.35, 0.10, 0.0});
    EXPECT_TRUE(c.carver);
    EXPECT_EQ(c.filled, (Mask{0, 0, 0, 1}));
    EXPECT_DOUBLE_EQ(c.bars_per_year, 256.0);
    expect_rel(c.matrix[0][0], 0.20 * 0.20);
    expect_rel(c.matrix[1][1], 0.35 * 0.35);
    expect_rel(c.matrix[0][1], 0.20 * 0.35);
    expect_rel(c.matrix[1][0], 0.20 * 0.35);
    expect_rel(c.matrix[0][2], -0.20 * 0.10);
    expect_rel(c.matrix[1][2], -0.35 * 0.10);
    EXPECT_DOUBLE_EQ(c.matrix[3][3], 0.01);
    EXPECT_DOUBLE_EQ(c.matrix[0][3], 0.0);
    EXPECT_DOUBLE_EQ(c.matrix[3][1], 0.0);

    const Covariance none = strategy25_covariance(r, Vector(3, 0.0), {0.20, 0.35, 0.10, 0.0});
    EXPECT_EQ(none.filled, (Mask{1, 1, 1, 1}));
    EXPECT_DOUBLE_EQ(none.bars_per_year, 0.0);
    EXPECT_DOUBLE_EQ(none.matrix[0][0], 0.01);
}

// b_sigma_rule (AFTS gap list C-1, C-2): row 3, the lumpiest contract, is held at 1 and searched to
// 0. Today's rule lets that held-only row set B_sigma (0.0447), the held book is inside its own
// buffer's reach (a = 0.023) and rounds back unchanged; strategy 25's fixed buffer (0.05 tau = 1%)
// exits it and opens row 2; the minimal variant leaves the held-only row out of the names, so the
// floor governs here, and with a lower floor the next-largest named row sets it.
TEST(OnePass, TheBufferWidthByRule) {
    const Fixture f = fixture();
    const Covariance c = optimiser_covariance(f.closes, f.levels, f.ordinals, kStale);
    const Vector searched = {3.0, -10.0, 20.0, 0.0, 2.0};
    auto [b, index] = b_sigma(f.u, c.matrix, searched, kHeld, 0.20, 0.05, BSigmaRule::kGranularity);
    expect_rel(b, 0.044722270829816);
    EXPECT_EQ(index, 3);
    Buffered buffered = buffer(searched, kHeld, f.u, c.matrix, b);
    expect_rel(buffered.te_held, 0.045768799673906464);
    expect_rel(buffered.a, 0.02286555145747257);
    EXPECT_TRUE(buffered.returned_to_held);
    EXPECT_EQ(buffered.book, kHeld);

    std::tie(b, index) = b_sigma(f.u, c.matrix, searched, kHeld, 0.20, 0.05, BSigmaRule::kFixed);
    EXPECT_DOUBLE_EQ(b, 0.05 * 0.20);
    EXPECT_EQ(index, -1);
    buffered = buffer(searched, kHeld, f.u, c.matrix, b);
    expect_rel(buffered.a, 0.7815105471140166);
    EXPECT_EQ(buffered.book, (Vector{3.0, -10.0, 16.0, 0.0, 2.0})) << "the held row 3 exits";

    std::tie(b, index) = b_sigma(f.u, c.matrix, searched, kHeld, 0.20, 0.10, BSigmaRule::kFixed);
    EXPECT_DOUBLE_EQ(b, 0.10 * 0.20) << "the fixed rule's width is the floor ratio";
    EXPECT_EQ(buffer(searched, kHeld, f.u, c.matrix, b).book, (Vector{3.0, -10.0, 11.0, 0.0, 2.0}));

    std::tie(b, index) = b_sigma(f.u, c.matrix, searched, kHeld, 0.20, 0.05, BSigmaRule::kMinimal);
    EXPECT_DOUBLE_EQ(b, 0.05 * 0.20) << "without row 3 no named row is above the floor";
    EXPECT_EQ(index, -1);
    std::tie(b, index) = b_sigma(f.u, c.matrix, searched, kHeld, 0.20, 0.001, BSigmaRule::kMinimal);
    expect_rel(b, 0.0013299492588549058);
    EXPECT_EQ(index, 1) << "the largest one-contract risk among the rows the search answers non-zero";
    std::tie(b, index) = b_sigma(f.u, c.matrix, searched, kHeld, 0.20, 0.001, BSigmaRule::kGranularity);
    EXPECT_EQ(index, 3) << "today's rule still names the held-only row";
}

// The whole pass with the forecast-sign close first: symbol 1 is held short against a forecast
// that points up, so it is closed to flat and the search, the cost term and the buffer start from
// the closed book.
TEST(OnePass, TheForecastSignCloseComesBeforeTheSearch) {
    const Fixture f = fixture();
    const Covariance c = optimiser_covariance(f.closes, f.levels, f.ordinals, kStale);
    const auto [closed_book, closed] = forecast_close(kHeld, {1.0, 1.0, 1.0, 1.0, 0.0});
    EXPECT_EQ(closed, (Mask{0, 1, 0, 0, 0}));
    EXPECT_EQ(closed_book, (Vector{3.0, 0.0, 0.0, 1.0, 2.0}));
    const Search s = search(kTarget, c.matrix, f.u, closed_book, kCost, 100.0, 2.0);
    EXPECT_EQ(s.book, (Vector{3.0, 0.0, 15.0, 6.0, 2.0}));
    expect_rel(s.tracking_error, 0.11442635662730244);
    EXPECT_EQ(s.passes, 20);
    const auto [b, b_index] = b_sigma(f.u, c.matrix, book_eligible(s.book, closed_book), 0.20);
    expect_rel(b, 0.044722270829816);
    const Buffered buffered = buffer(s.book, closed_book, f.u, c.matrix, b);
    expect_rel(buffered.te_held, 0.22779405288571972);
    expect_rel(buffered.a, 0.8036723511291475);
    EXPECT_EQ(buffered.book, (Vector{3.0, 0.0, 12.0, 5.0, 2.0}));
    const auto [clipped_book, clipped] = clip_to_cap(buffered.book, f.u, 2.0, {1, 1, 1, 1, 1});
    EXPECT_EQ(clipped_book, buffered.book);
    EXPECT_EQ(clipped, (Mask{0, 0, 0, 0, 0}));
}

TEST(OnePass, TheCapOnTheTargetAndTheClipOnTheBook) {
    const Fixture f = fixture();
    const auto [capped, bound] = cap_target({40.0, -300.0, 0.2, 3.0, -1.0}, f.u, 2.0);
    EXPECT_EQ(bound, (Mask{0, 1, 0, 0, 0}));
    expect_rel(capped[1], -185.7334042657189);
    EXPECT_DOUBLE_EQ(capped[0], 40.0);
    // the free rows are clipped toward zero to whole contracts; a held row (row 3 here) never is
    const auto [book, clipped] = clip_to_cap({40.0, -300.0, 1.0, 3.0, -1.0}, f.u, 2.0, {1, 1, 1, 0, 1});
    EXPECT_EQ(book, (Vector{40.0, -185.0, 1.0, 3.0, -1.0}));
    EXPECT_EQ(clipped, (Mask{0, 1, 0, 0, 0}));
}

TEST(OnePass, RoundingToNearestAHalfAwayFromZero) {
    EXPECT_DOUBLE_EQ(round_half_away(2.5), 3.0);
    EXPECT_DOUBLE_EQ(round_half_away(-2.5), -3.0);
    EXPECT_DOUBLE_EQ(round_half_away(0.49), 0.0);
    EXPECT_DOUBLE_EQ(round_half_away(-0.5), -1.0);
    EXPECT_DOUBLE_EQ(round_half_away(1.5), 2.0);
    EXPECT_DOUBLE_EQ(round_half_away(-1.49), -1.0);
}

// The step rules: only on the target's side or toward zero; a target of zero steps only toward
// zero; a step away from zero that lands strictly above the cap is refused, one toward zero never.
TEST(OnePass, TheAdmissibleSteps) {
    EXPECT_TRUE(admissible(1.0, 0.0, 1.0, 0.1, 2.0));
    EXPECT_FALSE(admissible(-1.0, 0.0, 1.0, 0.1, 2.0)) << "past zero against the target's sign";
    EXPECT_TRUE(admissible(-2.0, -3.0, 1.0, 0.1, 2.0)) << "toward zero is always a candidate";
    EXPECT_FALSE(admissible(1.0, 0.0, 0.0, 0.1, 2.0)) << "a target of zero has no side";
    EXPECT_TRUE(admissible(1.0, 2.0, 0.0, 0.1, 2.0));
    EXPECT_FALSE(admissible(21.0, 20.0, 1.0, 0.1, 2.0)) << "21 x 0.1 is above the cap";
    EXPECT_TRUE(admissible(20.0, 19.0, 1.0, 0.1, 2.0)) << "exactly at the cap is allowed";
    EXPECT_TRUE(admissible(24.0, 25.0, 1.0, 0.1, 2.0)) << "a row beyond the cap may come back";
}

// The trim removes one contract at a time from the candidate rows, the one that lowers the first
// breached reading most, toward zero on a short; at most five; a removal that lowers nothing stops.
TEST(OnePass, TheTrimServesTheFirstBreachedReading) {
    // readings of a three-symbol book on fixed weights per contract: gross and net leverage only
    const Vector u = {0.5, 0.3, 0.2};
    auto read = [&](const Vector& book) {
        overlay::Readings r;
        for (size_t i = 0; i < book.size(); ++i) {
            r.gross += std::abs(book[i]) * u[i];
            r.net += book[i] * u[i];
        }
        return r;
    };
    overlay::Limits limits;
    limits.risk = limits.jump = limits.shock = 1e9;
    limits.gross = 3.0;
    limits.net = 1e9;
    // gross 0.5 x 4 + 0.3 x 5 + 0.2 x 3 = 4.1: over by 1.1; removing one of symbol 0 lowers it most
    Trimmed t = trim({4.0, -5.0, 3.0}, {1, 1, 1}, read, limits);
    EXPECT_EQ(t.book, (Vector{1.0, -5.0, 3.0})) << "three contracts of symbol 0: 3.6, 3.1, 2.6";
    ASSERT_EQ(t.removed.size(), 3u);
    EXPECT_EQ(t.removed[0], (std::pair<std::size_t, std::string>{0, "L_g"}));
    EXPECT_EQ(t.removed[2].first, 0u);
    EXPECT_FALSE(t.capped);
    EXPECT_NEAR(t.readings.gross, 2.6, 1e-12);
    // only the short is a candidate: its contracts come off TOWARD ZERO (-5 to -1), four of them
    t = trim({4.0, -5.0, 3.0}, {0, 1, 0}, read, limits);
    EXPECT_EQ(t.book, (Vector{4.0, -1.0, 3.0}));
    EXPECT_EQ(t.removed.size(), 4u);
    EXPECT_NEAR(t.readings.gross, 2.9, 1e-12);
    EXPECT_NEAR(t.readings.net, 2.3, 1e-12);
    // only symbol 2 is a candidate (the others are held): five contracts cannot do it, and with
    // three contracts there the trim removes three and stops with the reading still over
    t = trim({4.0, -5.0, 3.0}, {0, 0, 1}, read, limits);
    EXPECT_EQ(t.book, (Vector{4.0, -5.0, 0.0}));
    EXPECT_EQ(t.removed.size(), 3u);
    EXPECT_FALSE(t.capped) << "no candidate is left: the trim stops, the remainder is marked by the caller";
    EXPECT_EQ(over_limit(t.readings, limits).size(), 1u);
    // at most five a day
    limits.gross = 0.1;
    t = trim({40.0, 0.0, 0.0}, {1, 1, 1}, read, limits);
    EXPECT_EQ(t.removed.size(), 5u);
    EXPECT_TRUE(t.capped);
    EXPECT_EQ(t.book[0], 35.0);
}

namespace {

// Four symbols, each contract a weight of 0.2 on 500,000, no window (the overlay is blind) and no
// closes (the optimiser's covariance is the guarded 0.01 diagonal): the day of
// tests/portfolio/test_one_pass_book.cpp, at the function itself.
DayInputs four_symbols() {
    DayInputs in;
    in.capital = 500000.0;
    in.limits = {0.45, 0.90, 0.80, 8.0, 6.0};
    const std::size_t n = 4;
    in.multiplier.assign(n, 1000.0);
    in.close.assign(n, 100.0);
    in.cost.assign(n, 5.0);
    in.held = {0.0, 4.0, 4.0, 3.0};
    in.target = {5.0, -5.0, -0.4, 6.0};
    in.first_forecast = {10.0, -10.0, -1.5, 10.0};
    in.signalling.assign(n, 1);
    in.first_signalling.assign(n, 1);
    in.hold = {0, 0, 0, 1};
    in.has_bar.assign(n, 1);
    in.ever_signalled.assign(n, 1);
    in.jump_sigma_daily.assign(n, 0.0);
    return in;
}

}  // namespace

// Section 6.4: held rows KEEP a reading over. Rows 2 and 3 are held long at a net weight of 7,
// over the net limit of 6 on their own; the two free rows are short, so the stored book's net is
// under the limit and no stored reading is over. No term is kept by hold, and no row is named.
TEST(OnePass, AByHoldTermIsKeptOnlyWhenTheStoredBookIsOverItToo) {
    DayInputs in = four_symbols();
    in.cap = 10.0;                 // no row is beyond the per-name cap (CAP is a mark of its own)
    in.limits.gross = 100.0;       // only the net leverage can be over
    in.hold = {0, 0, 1, 1};
    in.held = {0.0, 0.0, 20.0, 15.0};        // held rows: 4.0 + 3.0 = 7 of net weight
    in.target = {-5.0, -5.0, 20.0, 15.0};    // free rows: short one unit of weight each
    in.first_forecast = {-10.0, -10.0, 10.0, 10.0};
    const DayResult r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    EXPECT_EQ(r.fixed, (Mask{0, 0, 1, 1}));
    EXPECT_EQ(r.book[2], 20.0);
    EXPECT_EQ(r.book[3], 15.0);
    ASSERT_LT(r.book[0] + r.book[1], -5.0) << "the free rows are short at least 1.2 of weight";
    EXPECT_LT(r.stored_readings.net, 6.0) << "the stored book is inside the net limit";
    EXPECT_TRUE(r.over_limit.empty());
    EXPECT_TRUE(r.by_hold_terms.empty())
        << "the held rows alone read 7 of net, but the stored book is not over: no by-hold mark";
    EXPECT_EQ(r.by_hold, (Mask{0, 0, 0, 0}));

    // The same held rows with the free rows flat: the stored book IS over, and the term is kept.
    in.target = {0.0, 0.0, 20.0, 15.0};
    in.first_forecast = {0.0, 0.0, 10.0, 10.0};
    const DayResult over = rebalance(in);
    ASSERT_FALSE(over.over_limit.empty());
    EXPECT_EQ(over.over_limit.front().first, "L_n");
    ASSERT_EQ(over.by_hold_terms, (std::vector<std::string>{"L_n"}));
    EXPECT_EQ(over.by_hold, (Mask{0, 0, 1, 1}));
}

// Sections 5.2, 5.3, 6.1 and 6.3 composed: row 0 opens through the buffer, row 1 is closed by the
// forecast-sign close and re-opened, row 2 sits in the deferral band and is held, row 3 is in the
// caller's hold set and is held; the fills are the close and the move.
TEST(OnePass, TheDayIsClassifiedSearchedAndFilledOnce) {
    const DayInputs in = four_symbols();
    const DayResult r = rebalance(in);
    EXPECT_TRUE(r.refusal.empty());
    EXPECT_EQ(r.free, (Mask{1, 1, 0, 0}));
    EXPECT_EQ(r.band, (Mask{0, 0, 1, 0}));
    EXPECT_EQ(r.fixed, (Mask{0, 0, 1, 1}));
    EXPECT_EQ(r.sign_closed, (Mask{0, 1, 0, 0}));
    EXPECT_TRUE(r.window.blind());
    EXPECT_DOUBLE_EQ(r.multiplier.m, 1.0);
    EXPECT_EQ(r.book, (Vector{4.0, -4.0, 4.0, 3.0}));
    EXPECT_EQ(r.sign_fill, (Vector{0.0, -4.0, 0.0, 0.0}));
    EXPECT_EQ(r.rest_fill, (Vector{4.0, -4.0, 0.0, 0.0}));
    EXPECT_TRUE(r.searched);
    EXPECT_NEAR(r.b_sigma, 0.02, 1e-12);
    // the delivered scale: the stored gross over the gross of the capped target with the held rows
    EXPECT_NEAR(r.risk_scale, (0.8 + 0.8 + 0.8 + 0.6) / (1.0 + 1.0 + 0.8 + 0.6), 1e-12);

    const std::vector<std::string> symbols = {"AAA", "BBB", "CCC", "DDD"};
    const std::string overlay = overlay_line(symbols, in, r);
    EXPECT_NE(overlay.find("OVERLAY m=1 binding=none R=blind R_jump=blind R_shock=blind"), std::string::npos)
        << overlay;
    EXPECT_NE(overlay.find(" limits R_max=0.45 R_jump_max=0.9 R_shock_max=0.8 L_max=8 L_net_max=6 "),
              std::string::npos) << overlay;
    EXPECT_NE(overlay.find(" capital=500000 tau=0.2 window=blind dates=0 "), std::string::npos) << overlay;
    EXPECT_NE(overlay.find(" participants=4 free=2 held=2 "), std::string::npos) << overlay;
    const std::string optimiser = optimiser_line(symbols, r);
    EXPECT_EQ(optimiser.rfind("OPTIMISER searched=1 te=", 0), 0u) << optimiser;
    EXPECT_NE(optimiser.find(" b_sigma=0.02 "), std::string::npos) << optimiser;
    EXPECT_NE(optimiser.find(" traded=1 returned_to_held=0 free=2 sign_closes=[BBB] cap_clips=[-] "
                             "close_outs=[-] stale=["),
              std::string::npos) << optimiser;
    const std::string book = book_line(symbols, in, r, Mask{0, 0, 1, 0});
    EXPECT_NE(book.find(" held_count=2 band_holds=[CCC] slow_rule_zeroed=[CCC] rows=[AAA:5:5:0:4 "
                        "BBB:-5:-5:4:-4 CCC:0:0:4:4 DDD:0:0:3:3]"),
              std::string::npos) << book;
    const KeptLines kept = kept_lines(symbols, in, r);
    ASSERT_GE(kept.info.size(), 3u);
    EXPECT_EQ(kept.info[0].rfind("T4_RISK_WINDOW dates=0 symbols=4 ", 0), 0u) << kept.info[0];
    EXPECT_EQ(kept.warn.size(), 2u) << "both free rows have no close: each on the guarded diagonal";
}

// The rule reaches the pass: the day of TheDayIsClassifiedSearchedAndFilledOnce under the fixed
// rule sets B_sigma to the floor (0.05 x 0.20) with no symbol, against 0.02 set by a row today.
TEST(OnePass, TheRebalanceUsesTheBufferRule) {
    DayInputs in = four_symbols();
    DayResult r = rebalance(in);
    EXPECT_NEAR(r.b_sigma, 0.02, 1e-12);
    EXPECT_GE(r.b_symbol, 0);
    in.b_sigma_rule = BSigmaRule::kFixed;
    r = rebalance(in);
    EXPECT_NEAR(r.b_sigma, 0.01, 1e-12);
    EXPECT_EQ(r.b_symbol, -1);
}

// wrong_sign_rule "buffer" (AFTS gap list C-3): the day of TheDayIsClassifiedSearchedAndFilledOnce
// with no forecast-sign close and no deferral band. Row 1 (held 4 against a target of -5) is not
// closed: the search starts from 4 and the buffer moves it a = 0.909 of the way to -5, stored -4 in
// ONE fill of -8; row 2 (held 4, |F| 1.5 inside today's band) is free and moves to 0; B_sigma is
// set on the three free rows (0.02) and TE_h is over all three.
TEST(OnePass, AWrongSignedRowIsBufferedLikeAnyOther) {
    DayInputs in = four_symbols();
    in.wrong_sign_buffer = true;
    const DayResult r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    EXPECT_EQ(r.band, (Mask{0, 0, 0, 0}));
    EXPECT_EQ(r.free, (Mask{1, 1, 1, 0}));
    EXPECT_EQ(r.fixed, (Mask{0, 0, 0, 1}));
    EXPECT_EQ(r.sign_closed, (Mask{0, 0, 0, 0}));
    EXPECT_EQ(r.search_book, (Vector{5.0, -5.0, 0.0, 3.0}));
    EXPECT_NEAR(r.b_sigma, 0.02, 1e-12);
    EXPECT_NEAR(r.te_held, 0.22090722034374524, 1e-12);
    EXPECT_NEAR(r.a, 0.9094642539574815, 1e-12);
    EXPECT_EQ(r.book, (Vector{5.0, -4.0, 0.0, 3.0}));
    EXPECT_EQ(r.sign_fill, (Vector{0.0, 0.0, 0.0, 0.0}));
    EXPECT_EQ(r.rest_fill, (Vector{5.0, -8.0, -4.0, 0.0}));
}

// optimiser_covariance "carver" reaches the pass: the day of
// TheDayIsClassifiedSearchedAndFilledOnce with strategy 25's covariance on opt_sigma (no window: no
// common week, rho 0). Row 1's sigma 0.25
// gives a one-contract risk of 0.2 x 0.25 = 0.05, which sets B_sigma, against today's guarded 0.01
// variance (B_sigma 0.02); row 0 without a sigma is on the 0.01 diagonal.
TEST(OnePass, TheRebalanceUsesStrategyTwentyFivesCovariance) {
    DayInputs in = four_symbols();
    in.opt_carver = true;
    in.opt_sigma = {0.0, 0.25, 0.30, 0.40};
    const DayResult r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    EXPECT_TRUE(r.covariance.carver);
    ASSERT_EQ(r.free_rows, (std::vector<std::size_t>{0, 1}));
    EXPECT_EQ(r.covariance.filled, (Mask{1, 0}));
    EXPECT_DOUBLE_EQ(r.covariance.matrix[0][0], 0.01);
    EXPECT_NEAR(r.covariance.matrix[1][1], 0.0625, 1e-15);
    EXPECT_NEAR(r.b_sigma, 0.05, 1e-12);
    EXPECT_EQ(r.b_symbol, 1);
    EXPECT_EQ(r.stale, (Mask{0, 0}));
    in.opt_carver = false;
    EXPECT_NEAR(rebalance(in).b_sigma, 0.02, 1e-12) << "today: the guarded diagonal";
}

// Strategy 25's covariance reads the free rows' own columns of the overlay window: row 0 is held
// (not free) and its returns equal row 1's, rows 1 and 2 are free and move against each other every
// day, so their weekly correlation is -1 and the covariance is -0.2 x 0.3; a copy that took the
// first two columns would read +1. Row 1 without a sigma is filled and named in the warning.
TEST(OnePass, StrategyTwentyFivesCovarianceReadsTheFreeRowsColumns) {
    DayInputs in = four_symbols();
    in.hold = {1, 0, 0, 1};
    in.wrong_sign_buffer = true;  // row 2 is free (no deferral band)
    in.opt_carver = true;
    in.opt_sigma = {0.1, 0.2, 0.3, 0.4};
    unsigned state = 7u;
    for (int w = 0; w < 40; ++w) {
        for (int d = 0; d < 5; ++d) {
            state = state * 1103515245u + 12345u;
            const double x = static_cast<double>(state % 1000u) / 1000.0 - 0.5;
            in.ordinals.push_back(21004.0 + 7.0 * w + d);
            in.returns.push_back({0.01 * x, 0.01 * x, -0.02 * x, 0.005 * x});
        }
    }
    DayResult r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    ASSERT_EQ(r.free_rows, (std::vector<std::size_t>{1, 2}));
    EXPECT_EQ(r.covariance.filled, (Mask{0, 0}));
    expect_rel(r.covariance.matrix[0][0], 0.04);
    expect_rel(r.covariance.matrix[1][1], 0.09);
    expect_rel(r.covariance.matrix[0][1], -0.2 * 0.3);

    in.opt_sigma[1] = 0.0;
    r = rebalance(in);
    EXPECT_EQ(r.covariance.filled, (Mask{1, 0}));
    EXPECT_DOUBLE_EQ(r.covariance.matrix[0][0], 0.01);
    EXPECT_DOUBLE_EQ(r.covariance.matrix[0][1], 0.0);
    const KeptLines kept = kept_lines({"AAA", "BBB", "CCC", "DDD"}, in, r);
    bool named = false;
    for (const auto& line : kept.warn) {
        named = named || line.rfind("COVARIANCE_NO_SIGMA symbol=BBB", 0) == 0;
    }
    EXPECT_TRUE(named) << "the filled row is named";
    bool counted = false;
    for (const auto& line : kept.info) {
        counted = counted ||
                  line.rfind("COVARIANCE_STRATEGY25 symbols=1/2 bars_per_year=256 left_out=1", 0) == 0;
    }
    EXPECT_TRUE(counted);
}

// Section 4, the refusal: an input that is not a finite number, or a reading that is not, refuses
// the day. The book is the held book on every row and nothing is filled; a blind window is not a
// refusal.
TEST(OnePass, AnOverlayThatCannotAnswerRefusesTheDay) {
    DayInputs in = four_symbols();
    in.target[0] = std::numeric_limits<double>::quiet_NaN();
    DayResult r = rebalance(in);
    EXPECT_FALSE(r.refusal.empty());
    EXPECT_FALSE(r.refusal_on_reread);
    EXPECT_EQ(r.book, in.held);
    EXPECT_EQ(r.sign_fill, (Vector{0.0, 0.0, 0.0, 0.0}));
    EXPECT_EQ(r.rest_fill, (Vector{0.0, 0.0, 0.0, 0.0}));
    EXPECT_FALSE(r.searched);
    const std::vector<std::string> symbols = {"AAA", "BBB", "CCC", "DDD"};
    EXPECT_EQ(overlay_line(symbols, in, r).rfind("OVERLAY refused capital=500000 tau=0.2 ", 0), 0u);

    in = four_symbols();
    in.capital = 0.0;
    r = rebalance(in);
    EXPECT_FALSE(r.refusal.empty());
    EXPECT_EQ(r.book, in.held);

    // blind, and not refused
    in = four_symbols();
    r = rebalance(in);
    EXPECT_TRUE(r.window.blind());
    EXPECT_TRUE(r.refusal.empty());
}

// Section 6.4's marks as text: the terms still over after the trim with the largest excess in units
// of the largest stored weight per contract, and the held rows that keep the book over.
TEST(OnePass, TheTrimAndHoldMarksNameTheirTermsAndRows) {
    DayInputs in = four_symbols();
    // every row held at 30 contracts: a gross leverage of 24 the trim cannot touch, each row three
    // times the per-name cap
    in.hold.assign(4, 1);
    in.held.assign(4, 30.0);
    in.target.assign(4, 30.0);
    in.first_forecast.assign(4, 10.0);
    const DayResult r = rebalance(in);
    EXPECT_EQ(r.book, in.held) << "held rows are never cut";
    ASSERT_FALSE(r.over_limit.empty());
    EXPECT_EQ(r.over_limit.front().first, "L_g");
    ASSERT_EQ(r.over_limit.size(), 2u) << "gross 24 against 8 and net 24 against 6";
    EXPECT_EQ(r.over_limit.back().first, "L_n");
    // the LARGEST excess (the net's 18) in units of the largest stored weight per contract (0.2)
    EXPECT_NEAR(r.over_limit_excess_units, (24.0 - 6.0) / 0.2, 1e-9);
    ASSERT_FALSE(r.by_hold_terms.empty());
    EXPECT_EQ(r.by_hold_terms.back(), "CAP");
    EXPECT_EQ(r.by_hold, (Mask{1, 1, 1, 1}));
    const std::vector<std::string> symbols = {"AAA", "BBB", "CCC", "DDD"};
    EXPECT_EQ(risk_trim_line(symbols, r).rfind("RISK_TRIM over_limit_after_rounding terms=[L_g:16;L_n:18] "
                                               "excess_units=90 trimmed=[-] trim_capped=0", 0), 0u)
        << risk_trim_line(symbols, r);
    const std::string by_hold = over_limit_by_hold_line(symbols, r);
    EXPECT_EQ(by_hold.rfind("RISK_OVER_LIMIT_BY_HOLD terms=[", 0), 0u) << by_hold;
    EXPECT_NE(by_hold.find("CAP] symbols=[AAA BBB CCC DDD]"), std::string::npos) << by_hold;
}

// The record's carver readings: the capped target (with the held rows) and the stored book read on
// the carver covariance whatever the book's own rule. On a carver book they are its own readings;
// on a sample book they are what the carver book reads on the same target, and its own readings
// are the sample covariance's.
TEST(OnePass, TheCarverReadingsAreRecordedOnEitherRule) {
    DayInputs in = four_symbols();
    unsigned state = 99u;
    for (int w = 0; w < 30; ++w) {
        for (int d = 0; d < 5; ++d) {
            in.ordinals.push_back(21004.0 + 7.0 * w + d);
            Vector row(4, 0.0);
            for (auto& x : row) {
                state = state * 1103515245u + 12345u;
                x = 0.02 * (static_cast<double>(state % 1000u) / 1000.0 - 0.5);
            }
            in.returns.push_back(row);
        }
    }
    in.sizing_sigma = {0.10, 0.20, 0.15, 0.30};
    in.jump_sigma_daily = {0.01, 0.02, 0.015, 0.03};
    const DayResult sample = rebalance(in);
    in.overlay_carver = true;
    const DayResult carver = rebalance(in);
    ASSERT_TRUE(sample.refusal.empty() && carver.refusal.empty());
    ASSERT_FALSE(sample.window.blind());
    EXPECT_EQ(carver.carver_readings.risk, carver.readings.risk);
    EXPECT_EQ(carver.carver_stored_readings.risk, carver.stored_readings.risk);
    EXPECT_EQ(sample.carver_readings.risk, carver.readings.risk);
    EXPECT_EQ(sample.carver_readings.shock, carver.readings.shock);
    EXPECT_GT(std::abs(sample.readings.risk - carver.readings.risk), 1e-6)
        << "the sample covariance of these returns is not the sleeve's sigmas";
}

namespace {

// sigma_i sigma_j rho off the diagonal, sigma_i^2 on it
Matrix correlated(const Vector& sigma, double rho) {
    Matrix m(sigma.size(), Vector(sigma.size(), 0.0));
    for (std::size_t i = 0; i < sigma.size(); ++i) {
        for (std::size_t j = 0; j < sigma.size(); ++j) {
            m[i][j] = sigma[i] * sigma[j] * (i == j ? 1.0 : rho);
        }
    }
    return m;
}

using Steps = std::vector<std::pair<std::size_t, double>>;

}  // namespace

// buffer_step_rule (STEP): five one-contract moves with one-contract risks u sigma = 0.03, 0.02,
// 0.03, 0.03, 0.03 at rho 0.2; TE_h = 0.05. Under fraction, a shared a near 0.5 decides every move
// together: at B = 0.024 (a = 0.52) rows 0, 1, 2 and 3 all move, at B = 0.0255 (a = 0.49) none
// does. Under edge the same two buffers take the moves one at a time, the largest drop in the error
// first: row 0 up (error 0.04123), row 3 down (0.03), row 2 up (0.02, inside either buffer); row 1,
// the smallest risk, stays held. Hand computation: evidence/STEP/tooling/hand_edge.py.
TEST(OnePass, TheEdgeRuleTakesTheMovesOneAtATime) {
    const Vector u = {0.02, 0.01, 0.04, 0.01, 0.03};
    const Matrix c = correlated({1.5, 2.0, 0.75, 3.0, 1.0}, 0.2);
    const Vector held = {0.0, 1.0, -1.0, 2.0, 0.0};
    const Vector searched = {1.0, 0.0, 0.0, 1.0, 0.0};
    const Vector cost(5, 1.0);

    Buffered f = buffer(searched, held, u, c, 0.024);
    expect_rel(f.te_held, 0.05);
    expect_rel(f.a, 0.52);
    EXPECT_EQ(f.book, (Vector{1.0, 0.0, 0.0, 1.0, 0.0})) << "a = 0.52: four rows move together";
    EXPECT_TRUE(f.steps.empty());
    f = buffer(searched, held, u, c, 0.0255);
    expect_rel(f.a, 0.49);
    EXPECT_EQ(f.book, held) << "a = 0.49: the same four rows stay together";
    EXPECT_TRUE(f.returned_to_held);

    for (const double b : {0.024, 0.0255}) {
        const Buffered e = buffer(searched, held, u, c, b, BufferStepRule::kEdge, cost);
        EXPECT_TRUE(e.traded);
        expect_rel(e.te_held, 0.05);
        expect_rel(e.a, (0.05 - b) / 0.05);  // recorded as the fraction the book would have used
        EXPECT_EQ(e.book, (Vector{1.0, 1.0, 0.0, 1.0, 0.0})) << b;
        EXPECT_EQ(e.unrounded, e.book);
        EXPECT_EQ(e.steps, (Steps{{0, 1.0}, {3, -1.0}, {2, 1.0}})) << b;
        expect_rel(e.te_book, 0.02);
        EXPECT_TRUE(e.inside);
        EXPECT_FALSE(e.returned_to_held);
    }
}

// The edge rule stops as soon as the error is at or below B: four uncorrelated rows of risk 0.01,
// each held 3 short of its answer (TE_h = 0.01 x sqrt(36) = 0.06). Each step lowers the largest
// gap (ties to the row order); at B = 0.031 the gaps 1, 1, 2, 2 still read 0.01 sqrt(10) = 0.0316,
// so it takes one more step and stops at 1, 1, 1, 2: 0.01 sqrt(7) = 0.02646, one step short of the
// answer on rows 0 to 2 and two on row 3.
TEST(OnePass, TheEdgeRuleStopsInsideTheBuffer) {
    const Vector u(4, 0.01);
    const Matrix c = correlated({1.0, 1.0, 1.0, 1.0}, 0.0);
    const Vector held(4, 0.0);
    const Vector searched(4, 3.0);
    const Buffered e = buffer(searched, held, u, c, 0.031, BufferStepRule::kEdge, Vector(4, 1.0));
    expect_rel(e.te_held, 0.06);
    // the gaps go 3333 -> 2333 -> 2233 -> 2223 -> 2222 -> 1222 -> 1122 -> 1112
    EXPECT_EQ(e.book, (Vector{2.0, 2.0, 2.0, 1.0}));
    EXPECT_EQ(e.steps, (Steps{{0, 1.0}, {1, 1.0}, {2, 1.0}, {3, 1.0}, {0, 1.0}, {1, 1.0}, {2, 1.0}}));
    expect_rel(e.te_book, 0.01 * std::sqrt(7.0));
    EXPECT_TRUE(e.inside);
    EXPECT_LE(e.te_book, 0.031);
}

// No move lowers the error: two rows at rho 0.99, held +1 and -1 against answers of 0 (their errors
// offset: TE_h = 0.01 x sqrt(2 x 0.01) = 0.001414). Either one-contract move leaves the other's
// error unhedged (TE 0.01), so the book is held although TE_h is above B, and it is recorded so.
TEST(OnePass, TheEdgeRuleStopsWhenNoMoveLowersTheError) {
    const Vector u(2, 0.01);
    const Matrix c = correlated({1.0, 1.0}, 0.99);
    const Vector held = {1.0, -1.0};
    const Buffered e = buffer({0.0, 0.0}, held, u, c, 0.001, BufferStepRule::kEdge, {1.0, 1.0});
    expect_rel(e.te_held, 0.0014142135623730951);
    EXPECT_TRUE(e.traded);
    EXPECT_EQ(e.book, held);
    EXPECT_TRUE(e.steps.empty());
    EXPECT_FALSE(e.inside);
    EXPECT_TRUE(e.returned_to_held);
    expect_rel(e.te_book, e.te_held);
    EXPECT_EQ(buffer({0.0, 0.0}, held, u, c, 0.001).book, held) << "fraction: a = 0.29 rounds back";
}

// Ties in the error go to the lower cost per contract, then to the row order: four uncorrelated
// rows of equal risk 0.01, each one short of its answer, costs 3, 1, 2, 1. The order is row 1
// (cost 1, before row 3), row 3, row 2; at B = 0.0101 the error 0.01 of row 0 alone is inside.
TEST(OnePass, TheEdgeRulesTiesGoToTheCheaperContractThenTheRowOrder) {
    const Vector u(4, 0.01);
    const Matrix c = correlated({1.0, 1.0, 1.0, 1.0}, 0.0);
    const Buffered e = buffer(Vector(4, 1.0), Vector(4, 0.0), u, c, 0.0101, BufferStepRule::kEdge,
                              {3.0, 1.0, 2.0, 1.0});
    EXPECT_EQ(e.steps, (Steps{{1, 1.0}, {3, 1.0}, {2, 1.0}}));
    EXPECT_EQ(e.book, (Vector{0.0, 1.0, 1.0, 1.0}));
    // equal costs: the row order alone
    EXPECT_EQ(buffer(Vector(4, 1.0), Vector(4, 0.0), u, c, 0.0101, BufferStepRule::kEdge).steps,
              (Steps{{0, 1.0}, {1, 1.0}, {2, 1.0}}));
}

// A move never passes or leaves the search's answer. Row 0 is at its answer (0); at rho 0.9 a step
// of row 0 to -1 would hedge rows 1 and 2 and lower the error (by 0.0001 - 0.00072 in squares), but
// it is not a candidate. Row 2 is held on the wrong side (+1 against -1): it crosses zero and stops
// at -1, never beyond. No position the search did not ask for appears.
TEST(OnePass, TheEdgeRuleNeverPassesOrLeavesTheAnswer) {
    const Vector u(3, 0.01);
    const Matrix c = correlated({1.0, 1.0, 1.0}, 0.9);
    const Vector held = {0.0, 2.0, 1.0};
    const Vector searched = {0.0, 0.0, -1.0};
    const Buffered e = buffer(searched, held, u, c, 0.0, BufferStepRule::kEdge, Vector(3, 1.0));
    EXPECT_EQ(e.book, searched);
    EXPECT_EQ(e.steps, (Steps{{1, -1.0}, {2, -1.0}, {1, -1.0}, {2, -1.0}}));
    for (const auto& [row, step] : e.steps) EXPECT_NE(row, 0u);
    EXPECT_TRUE(e.inside) << "B = 0: the answer's error 0 is at B";
    EXPECT_DOUBLE_EQ(e.te_book, 0.0);
}

// The no-trade case is today's under both rules: TE_h at or below B holds the book, no step, a 0.
TEST(OnePass, TheNoTradeCaseIsTheSameUnderBothRules) {
    const Vector u(2, 0.01);
    const Matrix c = correlated({1.0, 1.0}, 0.0);
    for (const BufferStepRule rule : {BufferStepRule::kFraction, BufferStepRule::kEdge}) {
        const Buffered e = buffer({1.0, 0.0}, {0.0, 0.0}, u, c, 0.01, rule, {1.0, 1.0});
        EXPECT_FALSE(e.traded);
        EXPECT_EQ(e.book, (Vector{0.0, 0.0}));
        EXPECT_EQ(e.unrounded, (Vector{0.0, 0.0}));
        EXPECT_DOUBLE_EQ(e.a, 0.0);
        expect_rel(e.te_held, 0.01);
        EXPECT_TRUE(e.steps.empty());
        EXPECT_FALSE(e.returned_to_held);
    }
}

// The rule reaches the pass: the day of AWrongSignedRowIsBufferedLikeAnyOther (free rows 0 to 2
// held 0, 4, 4 and searched 5, -5, 0, one-contract risk 0.2 x 0.1 = 0.02) with B_sigma's floor at
// 0.25 tau = 0.05. TE_h = 0.02 sqrt(25 + 81 + 16) = 0.2209. Fraction (a = 0.7737) stores 4, -3, 1;
// edge steps the largest gap first (row 1 four times, then the gaps of rows 0 and 1 tie at 5 and go
// to row 0, and so on) until the squared gaps sum to 6 (TE 0.02 sqrt 6 = 0.04899): 4, -4, 2. The
// record carries the steps, the log names them, and the stored opt_config is unchanged.
TEST(OnePass, TheRebalanceUsesTheBufferStepRule) {
    DayInputs in = four_symbols();
    in.wrong_sign_buffer = true;
    in.b_sigma_floor = 0.25;
    DayResult r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    EXPECT_NEAR(r.b_sigma, 0.05, 1e-12);
    EXPECT_NEAR(r.a, 0.7736606348937036, 1e-12);
    EXPECT_EQ(r.book, (Vector{4.0, -3.0, 1.0, 3.0}));
    EXPECT_FALSE(r.edge);
    EXPECT_TRUE(r.edge_steps.empty());
    const std::vector<std::string> symbols = {"AAA", "BBB", "CCC", "DDD"};
    EXPECT_EQ(optimiser_line(symbols, r).find("buffer_step_rule"), std::string::npos);

    in.buffer_step_rule = BufferStepRule::kEdge;
    r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    EXPECT_TRUE(r.edge);
    EXPECT_NEAR(r.te_held, 0.22090722034374524, 1e-12);
    EXPECT_NEAR(r.a, 0.7736606348937036, 1e-12) << "a is recorded as the fraction would have used";
    EXPECT_EQ(r.search_book, (Vector{5.0, -5.0, 0.0, 3.0}));
    EXPECT_EQ(r.edge_book, (Vector{4.0, -4.0, 2.0, 3.0}));
    EXPECT_EQ(r.book, (Vector{4.0, -4.0, 2.0, 3.0}));
    EXPECT_EQ(r.rest_fill, (Vector{4.0, -8.0, -2.0, 0.0}));
    EXPECT_EQ(r.edge_steps,
              (Steps{{1, -1.0}, {1, -1.0}, {1, -1.0}, {1, -1.0}, {0, 1.0}, {1, -1.0}, {0, 1.0},
                     {1, -1.0}, {2, -1.0}, {0, 1.0}, {1, -1.0}, {2, -1.0}, {0, 1.0}, {1, -1.0}}));
    EXPECT_NEAR(r.edge_te, 0.04898979485566357, 1e-12);
    EXPECT_TRUE(r.edge_inside);
    const std::string optimiser = optimiser_line(symbols, r);
    EXPECT_NE(optimiser.find(" buffer_step_rule=edge edge_steps=14 edge_te=0.04898979486 "
                             "edge_stop=inside edge_order=[BBB-1 BBB-1 BBB-1 BBB-1 AAA+1 BBB-1 "
                             "AAA+1 BBB-1 CCC-1 AAA+1 BBB-1 CCC-1 AAA+1 BBB-1]"),
              std::string::npos)
        << optimiser;
    EXPECT_EQ(attribute(in, r), (std::vector<std::string>{"buffer", "buffer", "buffer", "hold"}));
}

// The edge steps are named by symbol, not by free row: with row 0 held (not free) the free rows are
// 1, 2 and 3, and every step names one of them, each symbol's steps summing to its move from the
// held book to the buffer's book (the held row's book is its held quantity).
TEST(OnePass, TheEdgeStepsNameTheSymbolsOfTheFreeRows) {
    DayInputs in = four_symbols();
    in.hold = {1, 0, 0, 0};
    in.held = {2.0, 4.0, 4.0, 0.0};
    in.wrong_sign_buffer = true;
    in.b_sigma_floor = 0.25;
    in.buffer_step_rule = BufferStepRule::kEdge;
    const DayResult r = rebalance(in);
    ASSERT_TRUE(r.refusal.empty()) << r.refusal;
    ASSERT_EQ(r.free_rows, (std::vector<std::size_t>{1, 2, 3}));
    ASSERT_FALSE(r.edge_steps.empty());
    Vector moved(4, 0.0);
    for (const auto& [symbol, step] : r.edge_steps) {
        ASSERT_LT(symbol, 4u);
        EXPECT_NE(symbol, 0u) << "row 0 is held";
        moved[symbol] += step;
    }
    EXPECT_EQ(r.edge_book[0], 2.0);
    for (std::size_t i = 1; i < 4; ++i) EXPECT_EQ(moved[i], r.edge_book[i] - in.held[i]) << i;
}

// The one-pass record under each rule: a fraction book's day file is today's (no edge column); an
// edge book's day rows, the no-pass row included, end with edge_steps, edge_order, edge_te and
// edge_stop, every row as wide as its header.
TEST(OnePass, TheRecordCarriesTheEdgeColumnsOnAnEdgeBookOnly) {
    const std::filesystem::path dir =
        std::filesystem::current_path() / ("onepass_edge_record_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ::setenv("TRADE_NGIN_SERIES_DUMP_DIR", dir.c_str(), 1);
    const std::vector<std::string> symbols = {"AAA", "BBB", "CCC", "DDD"};
    DayInputs in = four_symbols();
    in.wrong_sign_buffer = true;
    in.b_sigma_floor = 0.25;
    append_one_pass_record("FRACTION_BOOK", "2026-01-02", "2026-01-02", false, symbols, in,
                           rebalance(in));
    in.buffer_step_rule = BufferStepRule::kEdge;
    append_one_pass_record("EDGE_BOOK", "2026-01-02", "2026-01-02", false, symbols, in, rebalance(in));
    append_no_pass_record("EDGE_BOOK", "2026-01-05", "2026-01-05", false, true);
    ::unsetenv("TRADE_NGIN_SERIES_DUMP_DIR");
    auto lines = [&](const std::string& name) {
        std::ifstream f(dir / name);
        std::vector<std::string> out;
        for (std::string line; std::getline(f, line);) out.push_back(line);
        return out;
    };
    auto cells = [](const std::string& line) {  // every cell, the empty ones at the end too
        std::vector<std::string> out(1);
        for (const char ch : line) {
            if (ch == ',') {
                out.emplace_back();
            } else {
                out.back() += ch;
            }
        }
        return out;
    };
    const auto fraction = lines("onepass_days_FRACTION_BOOK.csv");
    ASSERT_EQ(fraction.size(), 2u);
    EXPECT_EQ(fraction[0], record_detail::days_header(false));
    EXPECT_EQ(fraction[0].find("edge"), std::string::npos);
    EXPECT_EQ(cells(fraction[1]).size(), cells(fraction[0]).size());

    const auto edge = lines("onepass_days_EDGE_BOOK.csv");
    ASSERT_EQ(edge.size(), 3u) << "the header, the day and the no-pass day";
    const auto header = cells(edge[0]);
    ASSERT_EQ(header.size(), cells(fraction[0]).size() + 4);
    EXPECT_EQ(std::vector<std::string>(header.end() - 4, header.end()),
              (std::vector<std::string>{"edge_steps", "edge_order", "edge_te", "edge_stop"}));
    const auto day = cells(edge[1]);
    ASSERT_EQ(day.size(), header.size());
    EXPECT_EQ(day[header.size() - 4], "14");
    EXPECT_EQ(day[header.size() - 3],
              "BBB-1;BBB-1;BBB-1;BBB-1;AAA+1;BBB-1;AAA+1;BBB-1;CCC-1;AAA+1;BBB-1;CCC-1;AAA+1;BBB-1");
    EXPECT_NEAR(std::stod(day[header.size() - 2]), 0.04898979485566357, 1e-15);
    EXPECT_EQ(day[header.size() - 1], "inside");
    const auto no_pass = cells(edge[2]);
    EXPECT_EQ(no_pass.size(), header.size());
    EXPECT_EQ(no_pass[4], "no_pass");
    std::filesystem::remove_all(dir);
}
