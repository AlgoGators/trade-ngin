// QT plan E5: one_pass::attribute names the step of the pass that moved each symbol from what was
// asked (the desk's quantity) to what was stored (rulings 10 and 12; contract section 5).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "trade_ngin/optimization/one_pass.hpp"

using namespace trade_ngin::one_pass;

namespace {

// One free row asked `asked`, held `held`, with every step of the pass passing it through
// unchanged; each test then changes the step it is about.
struct Row {
    DayInputs in;
    DayResult out;
};

Row passthrough(double asked, double held = 0.0) {
    Row r;
    r.in.target = {asked};
    r.in.held = {held};
    r.out.free = {1};
    r.out.sign_closed = {0};
    r.out.capped_target = {asked};
    r.out.scaled_target = {asked};
    r.out.search_book = {asked};
    r.out.traded = true;
    r.out.a = 1.0;  // the buffer takes the whole step: buffered == searched
    r.out.pre_trim = {asked};
    r.out.book = {asked};
    return r;
}

std::string moved(const Row& r) { return attribute(r.in, r.out).at(0); }

}  // namespace

TEST(OnePassAttribute, UnmovedRowIsNone) {
    EXPECT_EQ(moved(passthrough(3.0, 1.0)), "none");
    EXPECT_EQ(moved(passthrough(0.0, 0.0)), "none");
}

TEST(OnePassAttribute, HeldRowIsHold) {
    Row r = passthrough(3.0, 1.0);
    r.out.free = {0};
    r.out.book = {1.0};
    EXPECT_EQ(moved(r), "hold");
}

TEST(OnePassAttribute, RefusedDayIsHold) {
    Row r = passthrough(3.0, 1.0);
    r.out.refusal = "the overlay's readings are not finite";
    r.out.book = {1.0};
    EXPECT_EQ(moved(r), "hold");
}

TEST(OnePassAttribute, CapBindsIsCap) {
    Row r = passthrough(10.0);
    r.out.capped_target = {4.0};
    r.out.scaled_target = {4.0};
    r.out.search_book = {4.0};
    r.out.pre_trim = {4.0};
    r.out.book = {4.0};
    EXPECT_EQ(moved(r), "cap");
}

TEST(OnePassAttribute, OverlayScaleIsOverlay) {
    Row r = passthrough(10.0);
    r.out.scaled_target = {6.0};
    r.out.search_book = {6.0};
    r.out.pre_trim = {6.0};
    r.out.book = {6.0};
    EXPECT_EQ(moved(r), "overlay");
}

TEST(OnePassAttribute, SearchIsSearch) {
    Row r = passthrough(5.0);
    r.out.search_book = {2.0};
    r.out.pre_trim = {2.0};
    r.out.book = {2.0};
    EXPECT_EQ(moved(r), "search");
}

TEST(OnePassAttribute, NoTradeBufferIsBuffer) {
    // Inside the no-trade buffer the held book stands.
    Row r = passthrough(5.0, 4.0);
    r.out.traded = false;
    r.out.a = 0.0;
    r.out.pre_trim = {4.0};
    r.out.book = {4.0};
    EXPECT_EQ(moved(r), "buffer");
}

TEST(OnePassAttribute, PartialBufferStepIsBuffer) {
    Row r = passthrough(9.0, 1.0);
    r.out.a = 0.5;  // buffered 5.0 (moved 4), rounded 5.0
    r.out.pre_trim = {5.0};
    r.out.book = {5.0};
    EXPECT_EQ(moved(r), "buffer");
}

TEST(OnePassAttribute, RoundingIsRounding) {
    Row r = passthrough(1.0, 0.0);
    r.out.search_book = {0.6};  // the search moved 0.4
    r.out.a = 0.8;              // buffered 0.48 (moved 0.12), rounded 0 (moved 0.48)
    r.out.pre_trim = {0.0};
    r.out.book = {0.0};
    EXPECT_EQ(moved(r), "rounding");
}

TEST(OnePassAttribute, ARoundingBackToTheAskIsNone) {
    Row r = passthrough(3.0, 2.0);
    r.out.a = 0.9;  // buffered 2.9, rounded 3: stored == asked
    EXPECT_EQ(moved(r), "none");
}

TEST(OnePassAttribute, ClipIsClip) {
    Row r = passthrough(6.0);
    r.out.pre_trim = {4.0};
    r.out.book = {4.0};
    EXPECT_EQ(moved(r), "clip");
}

TEST(OnePassAttribute, TrimIsTrim) {
    Row r = passthrough(6.0);
    r.out.book = {5.0};
    EXPECT_EQ(moved(r), "trim");
}

TEST(OnePassAttribute, SignCloseToFlatIsSignClose) {
    Row r = passthrough(-2.0, 3.0);
    r.out.sign_closed = {1};
    r.out.search_book = {0.0};
    r.out.pre_trim = {0.0};
    r.out.book = {0.0};
    EXPECT_EQ(moved(r), "sign_close");
}

TEST(OnePassAttribute, TiesGoToTheEarlierStep) {
    Row r = passthrough(10.0);
    r.out.capped_target = {8.0};  // the cap moved 2
    r.out.scaled_target = {6.0};  // the overlay moved 2
    r.out.search_book = {6.0};
    r.out.pre_trim = {6.0};
    r.out.book = {6.0};
    EXPECT_EQ(moved(r), "cap");
}

TEST(OnePassAttribute, OneNamePerSymbolInOrder) {
    Row r;
    r.in.target = {1.0, 5.0, 2.0};
    r.in.held = {1.0, 0.0, 2.0};
    r.out.free = {1, 1, 0};
    r.out.sign_closed = {0, 0, 0};
    r.out.capped_target = {1.0, 3.0, 0.0};
    r.out.scaled_target = {1.0, 3.0, 0.0};
    r.out.search_book = {1.0, 3.0, 2.0};
    r.out.traded = true;
    r.out.a = 1.0;
    r.out.pre_trim = {1.0, 3.0, 2.0};
    r.out.book = {1.0, 3.0, 2.0};
    const auto names = attribute(r.in, r.out);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "none");
    EXPECT_EQ(names[1], "cap");
    EXPECT_EQ(names[2], "none");  // a held row the desk asked to keep as it is
}

TEST(OnePassAttribute, ARealPassOnATargetPastTheCapNamesTheCap) {
    // No overlay window (BLIND: the covariance multipliers are 1) and limits nobody reaches. One
    // contract of symbol 0 weighs 1000 x 100 / 500,000 = 0.2 of the capital, so the cap of 2.0
    // allows 10 and an ask of 50 is capped.
    DayInputs in;
    in.capital = 500000.0;
    in.cap = 2.0;
    in.sign_band = 0.0;
    in.limits = {1e9, 1e9, 1e9, 1e9, 1e9};
    in.multiplier = {1000.0, 1000.0};
    in.close = {100.0, 100.0};
    in.held = {0.0, 0.0};
    in.target = {50.0, 1.0};
    in.first_forecast = {10.0, 10.0};
    in.cost = {1.0, 1.0};
    in.signalling = {1, 1};
    in.first_signalling = {1, 1};
    in.hold = {0, 0};
    in.has_bar = {1, 1};
    in.ever_signalled = {1, 1};
    in.jump_sigma_daily = {0.0, 0.0};
    const DayResult out = rebalance(in);
    ASSERT_TRUE(out.refusal.empty()) << out.refusal;
    ASSERT_TRUE(out.cap_bound[0]);
    const auto names = attribute(in, out);
    EXPECT_EQ(names[0], "cap");
    EXPECT_LE(out.book[0], 10.0);
}
