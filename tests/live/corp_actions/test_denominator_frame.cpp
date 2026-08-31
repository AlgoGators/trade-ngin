// tests/live/corp_actions/test_denominator_frame.cpp
//
// FIX-2 + FIX-3: where the dividend denominator comes from.
//
// Two independent defects lived in the same block of the runner.
//
// FIX-3 (frame). The denominator map was built from `all_bars` -- ADJUSTED closes.
// The applier's per-event basis rescale has to equal
// compute_backward_adjustment_factors' per-event step, and that step is defined on RAW
// closes. An adjusted close already carries every LATER event in the window, so the two
// agree only when no later event exists. Under a stacked div-then-split catch-up batch
// the basis came out rescaled by 1 + split*d/c instead of 1 + d/c.
//
// FIX-2 (range). Deep holdings were topped up separately over [deep_start, window.start)
// -- and window.start EQUALS window.deep_start whenever there are deep symbols, because
// the globally-oldest inception is itself always a deep symbol. The half-open range
// collapsed to one day, so a holding older than the bulk load got its denominator from
// the wrong end of its history with no error raised.
//
// Both are now one raw-close read over the whole window, named by
// denominator_fetch_range().

#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/market_data_utils.hpp"
#include "trade_ngin/live/corp_action_window.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;
using trade_ngin::market_data_utils::AdjustmentBar;
using trade_ngin::market_data_utils::compute_backward_adjustment_factors;

namespace {

Position held(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    p.unrealized_pnl = Decimal(0.0);
    p.realized_pnl = Decimal(0.0);
    p.last_update = std::chrono::system_clock::now();
    return p;
}

CorpActionEvent dividend(const std::string& sym, const std::string& ex_date, double per_share,
                         double close_at_ex_date) {
    CorpActionEvent e;
    e.symbol = sym;
    e.ex_date = ex_date;
    e.type = CorpActionType::DIVIDEND;
    e.value = per_share;
    e.close_at_ex_date = close_at_ex_date;
    return e;
}

CorpActionEvent split(const std::string& sym, const std::string& ex_date, double factor) {
    CorpActionEvent e;
    e.symbol = sym;
    e.ex_date = ex_date;
    e.type = CorpActionType::SPLIT;
    e.value = factor;
    return e;
}

constexpr long kDay = 24 * 60 * 60;

}  // namespace

// ---------------------------------------------------------------------------
// FIX-2 -- the range must not collapse.
// ---------------------------------------------------------------------------

TEST(DenominatorRange, DeepHoldingGetsARangeSpanningItsHistoryNotOneDay) {
    // One holding established well before the 730-day bulk load.
    const std::time_t today = parse_ymd_utc("2026-08-31");
    std::unordered_map<std::string, std::string> inception = {{"OLD", "2018-01-15"}};

    auto w = derive_corp_action_window(today, /*min_days=*/14, /*bulk_days=*/730, inception);
    ASSERT_FALSE(w.deep_symbols.empty()) << "a 2018 inception is deep against a 730-day load";

    auto range = denominator_fetch_range(w, today);
    EXPECT_EQ(range.start, "2018-01-15");
    EXPECT_EQ(range.end, "2026-08-31");
    EXPECT_TRUE(range.spans_more_than_one_day());
}

TEST(DenominatorRange, TheCollapseTheOldTopUpSufferedIsStructural) {
    // Why the old [deep_start, window.start) range was always one day: the globally
    // oldest inception is itself a deep symbol, so whatever pushes deep_start back
    // pushes start back with it. Pinned so nobody reintroduces a range ending at
    // window.start.
    const std::time_t today = parse_ymd_utc("2026-08-31");
    std::unordered_map<std::string, std::string> inception = {
        {"OLD", "2018-01-15"}, {"OLDER", "2011-03-02"}, {"RECENT", "2026-06-01"}};

    auto w = derive_corp_action_window(today, 14, 730, inception);
    ASSERT_FALSE(w.deep_symbols.empty());
    EXPECT_EQ(w.start, w.deep_start)
        << "window.start == window.deep_start is what collapsed [deep_start, start)";
    EXPECT_EQ(format_ymd_utc(w.deep_start), "2011-03-02");

    // The fixed range is anchored on `today`, so it cannot degenerate this way.
    auto range = denominator_fetch_range(w, today);
    EXPECT_TRUE(range.spans_more_than_one_day());
    EXPECT_EQ(range.start, "2011-03-02");
}

TEST(DenominatorRange, ShallowBookStillGetsTheFloorWindow) {
    // No deep symbols: the range is the 14-day floor through today, still not a point.
    const std::time_t today = parse_ymd_utc("2026-08-31");
    std::unordered_map<std::string, std::string> inception = {{"NEW", "2026-08-20"}};

    auto w = derive_corp_action_window(today, 14, 730, inception);
    EXPECT_TRUE(w.deep_symbols.empty());

    auto range = denominator_fetch_range(w, today);
    EXPECT_EQ(range.start, format_ymd_utc(today - 14 * kDay));
    EXPECT_EQ(range.end, "2026-08-31");
    EXPECT_TRUE(range.spans_more_than_one_day());
}

// ---------------------------------------------------------------------------
// FIX-3 -- the frame must be raw.
// ---------------------------------------------------------------------------

TEST(DenominatorFrame, StackedDivThenSplitIsExactOnRawCloses) {
    // 10 sh @ $100 basis. $1 dividend goes ex at D1 with a RAW close of $100; a 2:1
    // split follows at D2. Correct basis: 100 / (1 + 1/100) / 2 = 49.5049...
    std::unordered_map<std::string, Position> positions;
    positions["X"] = held("X", 10.0, 100.0);

    std::vector<CorpActionEvent> events = {dividend("X", "2026-03-02", 1.0, /*raw close=*/100.0),
                                           split("X", "2026-03-09", 2.0)};

    CorporateActionsApplier::apply(positions, events);

    // Tolerance is Decimal's storage precision (~1e-8), not slack in the contract:
    // the two frames this test separates are 0.49 apart.
    const double expected = 100.0 / (1.0 + 1.0 / 100.0) / 2.0;  // 49.50495...
    EXPECT_NEAR(positions["X"].average_price.as_double(), expected, 1e-6);
    EXPECT_NEAR(positions["X"].quantity.as_double(), 20.0, 1e-6);
}

TEST(DenominatorFrame, AnAdjustedCloseWouldGiveTheWrongBasis) {
    // The defect, stated as arithmetic. `all_bars` closes are backward-adjusted, so the
    // D1 close arrives already divided by the LATER 2:1 split: $50, not $100. Feeding
    // that in rescales basis by 1 + 1/50 instead of 1 + 1/100.
    std::unordered_map<std::string, Position> positions;
    positions["X"] = held("X", 10.0, 100.0);

    std::vector<CorpActionEvent> events = {
        dividend("X", "2026-03-02", 1.0, /*adjusted close=*/50.0), split("X", "2026-03-09", 2.0)};

    CorporateActionsApplier::apply(positions, events);

    const double wrong = 100.0 / (1.0 + 1.0 / 50.0) / 2.0;      // 49.0196...
    const double correct = 100.0 / (1.0 + 1.0 / 100.0) / 2.0;   // 49.5049...
    EXPECT_NEAR(positions["X"].average_price.as_double(), wrong, 1e-6);
    EXPECT_GT(std::abs(wrong - correct), 0.4)
        << "the two frames must be far enough apart that this is worth guarding";
}

TEST(DenominatorFrame, DividendThenDividendStacksExactlyOnRawCloses) {
    // Second-order case: two dividends in one catch-up batch. With raw closes each step
    // is independent, so the product is exact.
    std::unordered_map<std::string, Position> positions;
    positions["X"] = held("X", 10.0, 100.0);

    std::vector<CorpActionEvent> events = {dividend("X", "2026-03-02", 1.0, 100.0),
                                           dividend("X", "2026-06-01", 2.0, 105.0)};

    CorporateActionsApplier::apply(positions, events);

    const double expected = 100.0 / (1.0 + 1.0 / 100.0) / (1.0 + 2.0 / 105.0);
    EXPECT_NEAR(positions["X"].average_price.as_double(), expected, 1e-6);
}

TEST(DenominatorFrame, ApplierProductEqualsTheSqlCumulativeFactorOverATwoEventWindow) {
    // The contract in one assertion: the applier's sequential per-event rescale must
    // equal compute_backward_adjustment_factors' cumulative factor -- the C++ mirror of
    // build_equity_adjusted_query -- over the SAME window. It holds only in the raw
    // frame, which is what the unified read now supplies.
    //
    // Four raw bars: $1 dividend goes ex on bar 1, 2:1 split takes effect on bar 2.
    std::vector<AdjustmentBar> bars = {
        {100.0, 0.0, 1.0},  // bar 0, before both events
        {100.0, 1.0, 1.0},  // bar 1: dividend ex-date, raw close 100
        {50.0, 0.0, 2.0},   // bar 2: split effective
        {51.0, 0.0, 1.0},   // bar 3: last bar, factor 1
    };
    auto factors = compute_backward_adjustment_factors(bars);
    ASSERT_EQ(factors.size(), 4u);
    ASSERT_NEAR(factors.back(), 1.0, 1e-12);

    // A basis established at bar 0 must end up divided by that bar's cumulative factor's
    // reciprocal -- i.e. scaled into the same frame the marks now live in.
    std::unordered_map<std::string, Position> positions;
    positions["X"] = held("X", 10.0, 100.0);
    std::vector<CorpActionEvent> events = {dividend("X", "2026-03-02", 1.0, 100.0),
                                           split("X", "2026-03-09", 2.0)};
    CorporateActionsApplier::apply(positions, events);

    EXPECT_NEAR(positions["X"].average_price.as_double(), 100.0 * factors[0], 1e-6)
        << "applier basis and SQL cumulative factor are in different frames";
}
