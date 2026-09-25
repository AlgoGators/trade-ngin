// T-7b-2 8b commit 3: the account's notional and posted margin on the NET per symbol.
//
// Both futures runners store gross_notional, net_notional and margin_posted (and from them
// equity_to_margin_ratio and the leverage columns) summed over the sleeves' rows, |q| per sleeve.
// On a symbol the sleeves hold on the same side that sum is the account's (|a| + |b| = |a + b|).
// On a symbol they hold on OPPOSITE sides it is not: TF +1 / FAST -1 posts margin on two contracts
// the broker never holds. HD 2026-09-19 / 2026-09-25 item 23: opposite sleeves are kept gross per
// sleeve for attribution while the ACCOUNT holds the net (no dollars tied up at the broker). So
// an opposed symbol's notional and margin are taken once, on the net; every other symbol keeps
// the per-sleeve accumulation, in the same order, bit for bit. active_positions still counts the
// sleeves' rows (unchanged).

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/book_exposure.hpp"

using namespace trade_ngin;

namespace {

using Books = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

Position pos(const std::string& s, double q, double px) {
    Position p;
    p.symbol = s;
    p.quantity = Decimal(q);
    p.average_price = Decimal(px);
    return p;
}

// ZT-like: 2,000 per point, 1,000 initial and 900 maintenance margin per contract;
// MES-like: 5 per point, 1,600 / 1,450.
double mult(const std::string& s) { return s == "ZT.v.0" ? 2000.0 : 5.0; }
double initial(const std::string& s) { return s == "ZT.v.0" ? 1000.0 : 1600.0; }
double maint(const std::string& s) { return s == "ZT.v.0" ? 900.0 : 1450.0; }

ExposurePriceFn price_fn() {
    return [](const std::string&, const Position& p) { return p.average_price.as_double(); };
}
ExposureNotionalFn notional_fn() {
    return [](const std::string& s, double q, double px) {
        return Result<double>(q * px * mult(s));
    };
}
ExposureMarginFn margin_fn() {
    return [](const std::string& s, double q, double) {
        return Result<std::pair<double, double>>(
            std::make_pair(std::fabs(q) * initial(s), std::fabs(q) * maint(s)));
    };
}

// The parent's loop, verbatim in its arithmetic and order (live_portfolio.cpp before 8b).
BookExposure parent_loop(const Books& books) {
    BookExposure out;
    for (const auto& [sid, pm] : books) {
        for (const auto& [s, p] : pm) {
            const double q = p.quantity.as_double();
            if (std::abs(q) < 1e-6) continue;
            out.active_positions++;
            const double px = p.average_price.as_double();
            const double n = q * px * mult(s);
            out.gross_notional += std::abs(n);
            out.net_notional += n;
            out.posted_margin += std::fabs(q) * initial(s);
            out.maintenance_margin += std::fabs(q) * maint(s);
        }
    }
    return out;
}

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

// Same-side sleeves (every BASE day in the gate windows): identical to the parent, bit for bit.
TEST(BookExposure, SameSideSleevesAreSummedExactlyAsBefore) {
    Books b{{"TREND_FOLLOWING", {{"ZT.v.0", pos("ZT.v.0", 2, 103.65234375)},
                                 {"MES.v.0", pos("MES.v.0", 1, 7192.5)}}},
            {"TREND_FOLLOWING_FAST", {{"ZT.v.0", pos("ZT.v.0", 1, 103.65234375)},
                                      {"MES.v.0", pos("MES.v.0", 3, 7192.5)}}}};
    const auto got = account_book_exposure(b, price_fn(), notional_fn(), margin_fn());
    const auto want = parent_loop(b);
    EXPECT_FALSE(got.failed);
    EXPECT_EQ(got.gross_notional, want.gross_notional);
    EXPECT_EQ(got.net_notional, want.net_notional);
    EXPECT_EQ(got.posted_margin, want.posted_margin);
    EXPECT_EQ(got.maintenance_margin, want.maintenance_margin);
    EXPECT_EQ(got.active_positions, want.active_positions);
    EXPECT_TRUE(got.net_lines.empty());
}

// The stored BASE rows of ZT.v.0 on 2025-11-08: TF +1, FAST -1. The broker holds nothing.
//   parent: gross 2 x 103.65234375 x 2,000 = 414,609.38, margin 2 x 1,000 = 2,000.
//   fixed:  net 0: gross 0, margin 0; the two rows still count as active positions.
TEST(BookExposure, OpposedSleevesThatCancelPostNoMargin) {
    Books b{{"TREND_FOLLOWING", {{"ZT.v.0", pos("ZT.v.0", 1, 103.65234375)}}},
            {"TREND_FOLLOWING_FAST", {{"ZT.v.0", pos("ZT.v.0", -1, 103.65234375)}}}};
    const auto got = account_book_exposure(b, price_fn(), notional_fn(), margin_fn());
    EXPECT_FALSE(got.failed);
    EXPECT_EQ(got.gross_notional, 0.0);
    EXPECT_EQ(got.net_notional, 0.0);
    EXPECT_EQ(got.posted_margin, 0.0);
    EXPECT_EQ(got.maintenance_margin, 0.0);
    EXPECT_EQ(got.active_positions, 2);
    ASSERT_EQ(got.net_lines.size(), 1u);
    EXPECT_NE(got.net_lines[0].find("BOOK_NET sym=ZT.v.0 net=0"), std::string::npos)
        << got.net_lines[0];
}

// A partial offset: TF +3, FAST -1 on ZT and a same-side MES pair. ZT is taken on its net of 2;
// MES as before.
//   parent: ZT gross 4 x 207,304.6875 = 829,218.75, margin 4,000.
//   fixed:  ZT gross 2 x 207,304.6875 = 414,609.375, margin 2,000; MES unchanged.
TEST(BookExposure, APartialOffsetIsTakenOnTheNet) {
    Books b{{"TREND_FOLLOWING", {{"ZT.v.0", pos("ZT.v.0", 3, 103.65234375)},
                                 {"MES.v.0", pos("MES.v.0", 1, 7192.5)}}},
            {"TREND_FOLLOWING_FAST", {{"ZT.v.0", pos("ZT.v.0", -1, 103.65234375)},
                                      {"MES.v.0", pos("MES.v.0", 1, 7192.5)}}}};
    const auto got = account_book_exposure(b, price_fn(), notional_fn(), margin_fn());
    EXPECT_FALSE(got.failed);
    const double zt = 103.65234375 * 2000.0, mes = 7192.5 * 5.0;
    EXPECT_DOUBLE_EQ(got.gross_notional, 2 * zt + 2 * mes);
    EXPECT_DOUBLE_EQ(got.net_notional, 2 * zt + 2 * mes);
    EXPECT_DOUBLE_EQ(got.posted_margin, 2 * 1000.0 + 2 * 1600.0);
    EXPECT_DOUBLE_EQ(got.maintenance_margin, 2 * 900.0 + 2 * 1450.0);
    EXPECT_EQ(got.active_positions, 4);
}

// A symbol the margin manager cannot price: the caller falls back to the combined figures, as
// the parent did, with the symbol and sleeve named and the rows counted up to the failure.
TEST(BookExposure, AFailureIsReportedForTheCallersFallback) {
    Books b{{"TREND_FOLLOWING", {{"XX.v.0", pos("XX.v.0", 1, 10.0)}}}};
    const auto got = account_book_exposure(
        b, price_fn(),
        [](const std::string& s, double, double) {
            return make_error<double>(ErrorCode::INVALID_DATA, "no instrument " + s, "test");
        },
        margin_fn());
    EXPECT_TRUE(got.failed);
    EXPECT_EQ(got.failed_symbol, "XX.v.0");
    EXPECT_EQ(got.failed_strategy, "TREND_FOLLOWING");
    EXPECT_EQ(got.active_positions, 1);
}

TEST(BookExposureSource, BothFuturesRunnersTakeTheAccountsExposureFromTheHelper) {
    std::string hunks[2];
    int i = 0;
    for (const char* f : {"apps/strategies/live_portfolio.cpp",
                          "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_source(f);
        if (src.empty()) GTEST_SKIP() << f << " not found";
        const auto a = src.find("const BookExposure exposure = account_book_exposure(");
        ASSERT_NE(a, std::string::npos) << f;
        const auto z = src.find("active_positions = exposure.active_positions;", a);
        ASSERT_NE(z, std::string::npos) << f;
        hunks[i++] = src.substr(a, z - a);
    }
    EXPECT_EQ(hunks[0], hunks[1]) << "the twins must compute the book's exposure identically";
}
