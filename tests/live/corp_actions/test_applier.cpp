// CorporateActionsApplier -- the arithmetic that applies a price-restating
// event to a held position: splits scale share count, dividends rescale cost
// basis, and neither may touch realized PnL.
//
// Consolidated from test_corp_actions_applier.cpp, test_corp_actions_frame_consistency.cpp,
// test_corp_actions_long_hold_total_return.cpp and the applier half of
// test_corp_actions_ultrareview_fixes.cpp -- files that were named after the
// review or incident that produced them rather than the component they cover.
//
// Contracts worth keeping in view: the dividend denominator is the EX-DATE
// close, which is what makes the basis rescale the exact inverse of the price
// series' adjustment factor (a prior-day close silently drifts from it); and
// date keys are UTC, because localtime on the deployed TZ=America/New_York
// image shifts every bar key a day.

#include <gtest/gtest.h>
#include <cmath>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corp_action_window.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include <ctime>
#include <string>
#include "trade_ngin/data/market_data_utils.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "trade_ngin/live/corporate_actions_audit_log.hpp"

// ---------------------------------------------------------------------------
// SUBJECT: core applier arithmetic -- a split multiplies quantity and divides
// basis so position VALUE is unchanged; a dividend rescales basis only. Also
// the skip paths: unknown symbol, zero quantity, malformed factors.
// from test_corp_actions_applier.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace applier_core {

using namespace trade_ngin;

// Phase 4 audit tests T4.1, T4.2, T4.5 (applier unit semantics).

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

CorpActionEvent split_event(const std::string& symbol, const std::string& date, double factor) {
    CorpActionEvent ev;
    ev.symbol = symbol;
    ev.ex_date = date;
    ev.type = CorpActionType::SPLIT;
    ev.value = factor;
    return ev;
}

CorpActionEvent dividend_event(const std::string& symbol, const std::string& date,
                               double per_share, double close_tm1) {
    CorpActionEvent ev;
    ev.symbol = symbol;
    ev.ex_date = date;
    ev.type = CorpActionType::DIVIDEND;
    ev.value = per_share;
    ev.close_at_ex_date = close_tm1;
    return ev;
}

}  // namespace

// T4.1: 4-for-1 split moves 100 AAPL @ $400 -> 400 @ $100. Notional preserved.
TEST(CorpActionsApplierTest, SplitFourForOne) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 400.0);

    auto log = CorporateActionsApplier::apply(
        positions, {split_event("AAPL", "2020-08-31", 4.0)});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].type, CorpActionType::SPLIT);
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 400.0);
    EXPECT_DOUBLE_EQ(positions["AAPL"].average_price.as_double(), 100.0);

    // Notional invariant (qty × avg_price) preserved.
    const double notional_before = 100.0 * 400.0;
    const double notional_after = positions["AAPL"].quantity.as_double() *
                                  positions["AAPL"].average_price.as_double();
    EXPECT_DOUBLE_EQ(notional_after, notional_before);
}

// T4.2: dividend rescales avg_price into the post-rescale closeadj frame.
// Position 100 AAPL bought at $100. $0.25 dividend, close[T-1]=$100.
// Expected ratio = 1 + 0.25/100 = 1.0025. Expected new avg_price = 100/1.0025 ≈ 99.751.
TEST(CorpActionsApplierTest, DividendRescaleAvgPrice) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions, {dividend_event("AAPL", "2024-08-12", 0.25, 100.0)});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].type, CorpActionType::DIVIDEND);
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 100.0);  // unchanged
    EXPECT_NEAR(positions["AAPL"].average_price.as_double(), 99.7506234, 1e-6);
    EXPECT_NEAR(log[0].ratio_change, 1.0025, 1e-6);
}

// Stacked events on the same symbol apply in input order.
// 100 AAPL @ $400 -> 4-for-1 split -> 400 @ $100 -> $0.25 div on $100 close ->
// 400 @ ~$99.7506.
TEST(CorpActionsApplierTest, StackedSplitThenDividend) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 400.0);

    auto log = CorporateActionsApplier::apply(
        positions,
        {split_event("AAPL", "2020-08-31", 4.0),
         dividend_event("AAPL", "2024-08-12", 0.25, 100.0)});

    ASSERT_EQ(log.size(), 2u);
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 400.0);
    EXPECT_NEAR(positions["AAPL"].average_price.as_double(), 99.7506234, 1e-6);
}

// ADR_SPLIT behaves identically to SPLIT.
TEST(CorpActionsApplierTest, AdrSplitMatchesSplit) {
    std::unordered_map<std::string, Position> positions;
    positions["FOO"] = make_position("FOO", 200.0, 50.0);

    CorpActionEvent ev;
    ev.symbol = "FOO";
    ev.ex_date = "2023-01-01";
    ev.type = CorpActionType::ADR_SPLIT;
    ev.value = 2.0;

    auto log = CorporateActionsApplier::apply(positions, {ev});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_DOUBLE_EQ(positions["FOO"].quantity.as_double(), 400.0);
    EXPECT_DOUBLE_EQ(positions["FOO"].average_price.as_double(), 25.0);
}

// Events for symbols absent from positions map are no-ops.
TEST(CorpActionsApplierTest, MissingSymbolIsSkipped) {
    std::unordered_map<std::string, Position> positions;
    positions["MSFT"] = make_position("MSFT", 100.0, 400.0);

    auto log = CorporateActionsApplier::apply(
        positions, {split_event("AAPL", "2020-08-31", 4.0)});

    EXPECT_TRUE(log.empty());
    EXPECT_DOUBLE_EQ(positions["MSFT"].quantity.as_double(), 100.0);
    EXPECT_TRUE(positions.find("AAPL") == positions.end());
}

// Zero-quantity positions are skipped.
TEST(CorpActionsApplierTest, ZeroQuantityIsSkipped) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 0.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions, {split_event("AAPL", "2020-08-31", 4.0)});

    EXPECT_TRUE(log.empty());
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(positions["AAPL"].average_price.as_double(), 100.0);
}

// Invalid split factors are skipped with a log warning (not a crash).
TEST(CorpActionsApplierTest, InvalidSplitFactorSkipped) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 400.0);

    auto log = CorporateActionsApplier::apply(
        positions, {split_event("AAPL", "2020-08-31", 0.0)});
    EXPECT_TRUE(log.empty());
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 100.0);
}

// Invalid dividend inputs (zero close[T-1] or zero amount) are skipped.
TEST(CorpActionsApplierTest, InvalidDividendInputsSkipped) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    // close[T-1] == 0
    auto log1 = CorporateActionsApplier::apply(
        positions, {dividend_event("AAPL", "2024-08-12", 0.25, 0.0)});
    EXPECT_TRUE(log1.empty());

    // amount == 0
    auto log2 = CorporateActionsApplier::apply(
        positions, {dividend_event("AAPL", "2024-08-12", 0.0, 100.0)});
    EXPECT_TRUE(log2.empty());

    EXPECT_DOUBLE_EQ(positions["AAPL"].average_price.as_double(), 100.0);
}

// type_from_action_string parses the equities_data.corporate_action.action
// text values that the live app passes in.
TEST(CorpActionsApplierTest, TypeFromActionString) {
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string("split"),
              CorpActionType::SPLIT);
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string("adrratiosplit"),
              CorpActionType::ADR_SPLIT);
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string("dividend"),
              CorpActionType::DIVIDEND);
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string("spinoff"),
              CorpActionType::UNKNOWN);
    EXPECT_EQ(CorporateActionsApplier::type_from_action_string(""),
              CorpActionType::UNKNOWN);
}

}  // namespace applier_core

// ---------------------------------------------------------------------------
// SUBJECT: the applier's dividend denominator must match the price series.
// Both use the ex-date close, so basis adjustment and mark move together --
// a mismatch here silently biases every dividend.
// from test_corp_actions_frame_consistency.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace applier_frame {

using namespace trade_ngin;

// A dividend is handled in two independent places that must agree:
//
//   * the PRICE series, which scales every pre-dividend bar by
//     close_D / (close_D + div_D)   -- build_equity_adjusted_query, mirrored by
//     compute_backward_adjustment_factors;
//   * the POSITION applier, which rescales cost basis so the basis stays in the
//     same frame as the marks it is compared against.
//
// If the applier divides by a different close than the price series does, basis
// and mark drift apart a little on every dividend -- silently, and permanently,
// because no later event repairs a basis. These tests pin the two to the same
// denominator: the close ON the ex-date.
//
// This is a different axis from the raw-dollar / adjusted-close frame mix that
// 05-22 §B6 documents as deliberate and load-bearing. That mix is preserved.
namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

}  // namespace

TEST(CorpActionFrameConsistency, DividendBasisRescaleMatchesPriceSeriesFactor) {
    // Three bars; a dividend goes ex on the middle one.
    const double close_before_ex = 101.00;
    const double close_on_ex = 100.00;  // post-drop close, the price series' denominator
    const double dividend = 0.50;

    std::vector<market_data_utils::AdjustmentBar> bars = {
        {close_before_ex, 0.0, 1.0},
        {close_on_ex, dividend, 1.0},
        {100.75, 0.0, 1.0},
    };
    const auto factors = market_data_utils::compute_backward_adjustment_factors(bars);
    ASSERT_EQ(factors.size(), 3u);

    // What the price series does to a pre-dividend bar.
    const double price_series_factor = factors[0];
    ASSERT_GT(price_series_factor, 0.0);

    // What the applier does to a pre-dividend cost basis.
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 90.0);

    CorpActionEvent ev;
    ev.symbol = "AAPL";
    ev.ex_date = "2026-08-10";
    ev.type = CorpActionType::DIVIDEND;
    ev.value = dividend;
    // BA-5 / C-1 D1: the ex-date close is SELECTED here from the same bar series the
    // price-adjustment factors were computed from, rather than being assigned the
    // already-correct literal. Handing the applier `close_on_ex` by name made the
    // fixture supply the fix, so the test could not distinguish the corrected frame
    // from the old one -- it asserted an equality it had itself arranged.
    ev.close_at_ex_date = bars[1].close;  // the bar the dividend goes ex on
    ASSERT_DOUBLE_EQ(ev.close_at_ex_date, close_on_ex) << "sanity: bars[1] IS the ex-date bar";

    const auto adjustments = CorporateActionsApplier::apply(positions, {ev});
    ASSERT_EQ(adjustments.size(), 1u);

    // The adjustment record must report the same ratio it applied, so a reader of the
    // audit log and the position row cannot disagree about what happened.
    EXPECT_NEAR(adjustments[0].ratio_change, 1.0 + dividend / bars[1].close, 1e-9);

    const double basis_factor = positions["AAPL"].average_price.as_double() / 90.0;

    // Pinned against the rule itself, computed here from the event fields alone. This
    // is what catches the two frames drifting TOGETHER.
    EXPECT_NEAR(basis_factor, 1.0 / (1.0 + dividend / bars[1].close), 1e-8);

    // The two must scale by the same amount. Decimal carries 8 decimal places,
    // so compare at that resolution rather than machine epsilon.
    EXPECT_NEAR(basis_factor, price_series_factor, 1e-8)
        << "cost basis and the price series must land in the same frame; "
           "basis_factor=" << basis_factor
        << " price_series_factor=" << price_series_factor;
}

TEST(CorpActionFrameConsistency, UsingThePriorDaysCloseWouldDriftFromThePriceSeries) {
    // BA-5 / C-1 D1: this used to compute `wrong_factor` and `price_series_factor`
    // from two local expressions and assert they differed. Both were arithmetic on
    // literals declared three lines above -- no repository code was involved, so it
    // could not fail however the applier changed. It now drives the APPLIER with each
    // denominator and asserts which one lands in the price series' frame.
    const double close_before_ex = 101.00;
    const double close_on_ex = 100.00;
    const double dividend = 0.50;
    const double basis = 90.0;

    std::vector<market_data_utils::AdjustmentBar> bars = {
        {close_before_ex, 0.0, 1.0}, {close_on_ex, dividend, 1.0}, {100.75, 0.0, 1.0}};
    const double price_series_factor =
        market_data_utils::compute_backward_adjustment_factors(bars)[0];

    // Run the real applier twice, changing ONLY the close it is handed.
    auto basis_after = [&](double close_used) {
        std::unordered_map<std::string, Position> positions;
        positions["AAPL"] = make_position("AAPL", 100.0, basis);
        CorpActionEvent ev;
        ev.symbol = "AAPL";
        ev.ex_date = "2026-08-10";
        ev.type = CorpActionType::DIVIDEND;
        ev.value = dividend;
        ev.close_at_ex_date = close_used;
        const auto adj = CorporateActionsApplier::apply(positions, {ev});
        EXPECT_EQ(adj.size(), 1u);
        return positions["AAPL"].average_price.as_double();
    };

    const double with_ex_date_close = basis_after(close_on_ex);
    const double with_prior_close = basis_after(close_before_ex);

    // The ex-date close is the price series' own denominator, so only it agrees.
    EXPECT_NEAR(with_ex_date_close / basis, price_series_factor, 1e-8)
        << "the ex-date close must put the basis in the price series' frame";
    EXPECT_GT(std::abs(with_prior_close / basis - price_series_factor), 1e-9)
        << "close[T-1] and close[ex-date] must not be interchangeable in the applier";

    // And the applier's rule is exactly 1 + d/close, measured independently of it.
    EXPECT_NEAR(with_ex_date_close, basis / (1.0 + dividend / close_on_ex), 1e-8);
    EXPECT_NEAR(with_prior_close, basis / (1.0 + dividend / close_before_ex), 1e-8);
}

// Date keys are built from UTC instants. On the deployed image
// (TZ=America/New_York) localtime renders a UTC-midnight timestamp as the
// PREVIOUS day, which shifted every corp-action key and the trading date the
// run writes under. This pins UTC rendering regardless of the host's zone.
TEST(CorpActionFrameConsistency, DateKeysAreUtcRegardlessOfHostTimezone) {
    // BA-5 / C-1 D1: this used to declare its OWN gmtime_r+strftime lambda and assert
    // that the lambda was timezone-independent. That tests libc, not this repository:
    // every date-key site could have been reverted to localtime and the test would
    // still pass. It now exercises format_ymd_utc, the primitive the corp-action
    // window and the runner's date keys actually go through.
    //
    // 2026-08-10 00:00:00 UTC.
    std::tm utc{};
    utc.tm_year = 126;
    utc.tm_mon = 7;
    utc.tm_mday = 10;
    const std::time_t t = timegm(&utc);

    const std::string before = format_ymd_utc(t);

    const char* saved_tz = std::getenv("TZ");
    setenv("TZ", "America/New_York", 1);
    tzset();
    const std::string under_eastern = format_ymd_utc(t);
    if (saved_tz) {
        setenv("TZ", saved_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    EXPECT_EQ(before, "2026-08-10");
    EXPECT_EQ(under_eastern, "2026-08-10")
        << "date keys must not shift with the host timezone; localtime here "
           "would yield 2026-08-09 and mis-key every corp-action lookup";

    // The round trip the window derivation depends on: a formatted key must parse
    // back to the instant it came from.
    EXPECT_EQ(parse_ymd_utc(format_ymd_utc(t)), t);
}

}  // namespace applier_frame

// ---------------------------------------------------------------------------
// SUBJECT: a position held across many events accumulates them correctly,
// i.e. repeated application composes rather than drifting.
// from test_corp_actions_long_hold_total_return.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace applier_long_hold {

using namespace trade_ngin;

// Phase 4 audit test T4.3 — long-hold total-return regression guard for §1.15.
//
// Scenario: held 100 shares of a 4%-annual-yield equity for 1 year with no
// net price change. Provider uses backward (retroactive) closeadj
// adjustment, so each dividend rescales the closeadj curve. Pre-fix, the
// stored avg_price never updated, accumulating ~4% understatement of return.
// Post-fix, the avg_price /= (1 + d/close_at_ex_date) rescaling per
// dividend keeps the avg_price in the same closeadj frame as bar.close,
// so MTM correctly reflects total return.
//
// We simulate 4 quarterly dividends of $1 (total $4) on a $100 stock held
// at 100 shares for the year. End-of-year closeadj = $100 (provider has
// retroactively rescaled the original purchase price down by the same
// cumulative factor). Pre-fix: avg_price still $100, MTM = (100-100)*100 = $0
// (silently dropping the $400 of return). Post-fix: avg_price /= each
// quarter's ratio, ends ≈ $96.117, MTM = (100-96.117)*100 ≈ $388 ≈ 4%.

TEST(CorpActionsLongHoldTotalReturnTest, FourQuarterlyDividendsCaptureTotalReturn) {
    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = "DIVCO";
    p.quantity = Quantity(100.0);
    p.average_price = Decimal(100.0);
    positions["DIVCO"] = p;

    // 4 quarterly dividends of $1.00 each, close[T-1] = $100 each quarter
    // (no net price change over the year).
    std::vector<CorpActionEvent> events;
    for (int q = 0; q < 4; ++q) {
        CorpActionEvent ev;
        ev.symbol = "DIVCO";
        ev.ex_date = "2025-" + std::to_string(3 * (q + 1)) + "-15";
        ev.type = CorpActionType::DIVIDEND;
        ev.value = 1.00;
        ev.close_at_ex_date = 100.0;
        events.push_back(ev);
    }

    auto log = CorporateActionsApplier::apply(positions, events);
    ASSERT_EQ(log.size(), 4u);

    // Each dividend scales avg_price by 1/(1 + 1/100) = 1/1.01.
    // After 4 quarters: avg_price = 100 / 1.01^4 = 100 / 1.04060401 ≈ 96.0980
    const double expected_avg = 100.0 / std::pow(1.01, 4);
    EXPECT_NEAR(positions["DIVCO"].average_price.as_double(), expected_avg, 1e-6);

    // MTM at year-end close $100 reflects the accumulated total return.
    const double mtm = (100.0 - positions["DIVCO"].average_price.as_double()) *
                       positions["DIVCO"].quantity.as_double();
    // 4% annual yield on 100 shares × $100 = $400 nominal, but the
    // compound-rescale formula gives slightly less than nominal:
    // mtm = (100 - 100/1.01^4) * 100 ≈ 100 - 96.0980) * 100 ≈ 390.20
    EXPECT_NEAR(mtm, 100.0 * (100.0 - expected_avg), 1e-6);
    EXPECT_GT(mtm, 380.0)
        << "Total return should be ~$390 (close to nominal 4% on $10K). "
           "Pre-fix MTM would be exactly $0 -- §1.15 silent understatement.";
    EXPECT_LT(mtm, 400.0);

    // Sanity: quantity is unchanged (dividends don't change share count).
    EXPECT_DOUBLE_EQ(positions["DIVCO"].quantity.as_double(), 100.0);
}

}  // namespace applier_long_hold

// ---------------------------------------------------------------------------
// SUBJECT: quantity as of the ex-date -- the applier must size the dividend
// on shares actually held that day, not on today's (possibly changed) size.
// from ur_applier.cpp
// Wrapped in a namespace so its file-local helpers cannot collide with the
// other sections; gtest test identities are unaffected.
// ---------------------------------------------------------------------------
namespace applier_ex_date_qty {

using namespace trade_ngin;

// Ultrareview PR #38 fix verification.
// Pins the three behaviors that ultrareview flagged as real correctness or
// durability gaps and we resolved in this PR:
//   - bug_021 (cash-basis qty): the audit log records ex_date qty, not today's.
//   - bug_003 (atomic save):    the on-disk state file is written via tmp+rename.
// bug_007, merged_bug_001, bug_013, bug_037, bug_002 are wire-up concerns in
// the live equity app's main() and are exercised via integration / manual
// verification (the unit-test surface here is just the pure-logic layer).

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    return p;
}

CorpActionEvent dividend_event(const std::string& symbol,
                               const std::string& date,
                               double per_share,
                               double close_tm1,
                               double qty_at_ex = 0.0) {
    CorpActionEvent ev;
    ev.symbol = symbol;
    ev.ex_date = date;
    ev.type = CorpActionType::DIVIDEND;
    ev.value = per_share;
    ev.close_at_ex_date = close_tm1;
    ev.qty_at_ex_date = qty_at_ex;
    return ev;
}

}  // namespace

// bug_021: cash-flow figure uses ex_date qty (not today's), so a catch-up
// run days after the ex_date still records the correct dividend cash.
//
// Setup: position is 100 today, but on ex_date the operator was holding 200.
// $0.50 dividend per share. The cash recorded should be 200*0.50 = $100,
// NOT 100*0.50 = $50.
TEST(CorpActionsUltrareviewFixes, DividendUsesQtyAtExDateForCashBasis) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions,
        {dividend_event("AAPL", "2024-08-12", 0.50, 100.0, /*qty_at_ex=*/200.0)});

    ASSERT_EQ(log.size(), 1u);
    // The recorded adjustment's quantity reflects ex_date holding (200), not
    // today's live position (100). avg_price math still uses the live position
    // (dividends don't change qty, only rescale avg_price into closeadj frame).
    EXPECT_DOUBLE_EQ(log[0].quantity_before, 200.0);
    EXPECT_DOUBLE_EQ(log[0].quantity_after, 200.0);
    // Position quantity in-memory is unchanged (qty doesn't change on dividend).
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 100.0);
}

// bug_021 regression guard: when the live app couldn't reconstruct ex_date qty
// (first-week catch-up, missing historical row, etc.) and leaves qty_at_ex_date
// at 0.0, the applier falls back to the live position quantity -- preserving
// pre-fix behavior so no audit log diff for the common same-day path.

TEST(CorpActionsUltrareviewFixes, DividendFallsBackToCurrentQtyWhenExDateQtyUnset) {
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions,
        {dividend_event("AAPL", "2024-08-12", 0.50, 100.0, /*qty_at_ex=*/0.0)});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_DOUBLE_EQ(log[0].quantity_before, 100.0);
    EXPECT_DOUBLE_EQ(log[0].quantity_after, 100.0);
}

// bug_021 cash-flow round-trip: the audit log uses the applier's recorded
// quantity_after as the dividend basis when computing total_cash for
// total_cumulative_dividend_income(). With qty_at_ex_date=200 and
// $0.50/share, total_cash should be $100, not $50.

}  // namespace applier_ex_date_qty

namespace unresolvable_denominator {
using namespace applier_core;

// A dividend whose ex-date close could not be established must be SKIPPED, and
// the skip must leave NO PositionAdjustment behind.
//
// This is load-bearing, not cosmetic. The runner used to fall back to today's
// T-1 close when no historical close was available, which sails past the
// applier's positive-close guard and applies the dividend against the wrong
// denominator: DD's 2025-11-03 dividend of $47.50 against its ex-date close of
// $34.69 is a ratio of 2.3693, but against a later close of $136.98 it is
// 1.3468 -- a true basis of $100 persisted as $175.92. The fix removes that
// fallback so the close arrives here as 0.0 and the event is skipped.
//
// The skip is only recoverable because CorporateActionsAuditLog::record() takes
// a PositionAdjustment: no adjustment means no trading.corp_action_applied row,
// which means is_applied() does not block the event and a later run retries it
// once price history covers the ex-date. If the applier ever starts emitting an
// adjustment for a skipped event, the dedup row would pin an unapplied dividend
// permanently -- so these assertions guard the mechanism, not just the arithmetic.

TEST(CorpActionsApplierTest, DividendWithNoEstablishedCloseIsSkippedAndLeavesNoAdjustment) {
    std::unordered_map<std::string, Position> positions;
    positions["DD"] = make_position("DD", 100.0, 100.0);

    auto log = CorporateActionsApplier::apply(
        positions, {dividend_event("DD", "2025-11-03", 47.50, /*close_tm1=*/0.0)});

    EXPECT_TRUE(log.empty())
        << "a skipped dividend must produce no adjustment, or the dedup row would "
           "pin it as applied and it could never be retried";
    EXPECT_DOUBLE_EQ(positions["DD"].average_price.as_double(), 100.0)
        << "cost basis must be left untouched, not rescaled by a fabricated ratio";
    EXPECT_DOUBLE_EQ(positions["DD"].quantity.as_double(), 100.0);
}

// The specific corruption the fix prevents, stated as arithmetic: had the
// runner's old fallback supplied a later close, the basis would have been
// rescaled by the wrong ratio. Pins the correct denominator's result so a
// regression shows up as a number, not a philosophy.
TEST(CorpActionsApplierTest, DividendUsesTheExDateCloseNotALaterOne) {
    std::unordered_map<std::string, Position> correct;
    correct["DD"] = make_position("DD", 100.0, 100.0);
    CorporateActionsApplier::apply(
        correct, {dividend_event("DD", "2025-11-03", 47.50, /*ex-date close=*/34.69)});

    std::unordered_map<std::string, Position> wrong;
    wrong["DD"] = make_position("DD", 100.0, 100.0);
    CorporateActionsApplier::apply(
        wrong, {dividend_event("DD", "2025-11-03", 47.50, /*a later close=*/136.98)});

    // 100 / (1 + 47.50/34.69) = 42.207 ; 100 / (1 + 47.50/136.98) = 74.252
    EXPECT_NEAR(correct["DD"].average_price.as_double(), 42.21, 0.01);
    EXPECT_NEAR(wrong["DD"].average_price.as_double(), 74.252, 0.01);
    EXPECT_GT(std::abs(wrong["DD"].average_price.as_double() -
                       correct["DD"].average_price.as_double()),
              30.0)
        << "the denominator's date is worth ~76% of the cost basis on this event";
}

}  // namespace unresolvable_denominator
