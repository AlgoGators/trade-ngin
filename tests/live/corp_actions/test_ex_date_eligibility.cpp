// E2-F17 regression pins — a corporate action must not restate a position the book did not
// hold on the ex-date.
//
// THE FAILURE, measured over an 82-day live replay (2026-04-10 .. 2026-06-30), the same run
// that validated the E2-F15 horizon gate. FOUR of the six dividends applied were applied to
// positions held on no part of their ex-date:
//
//   symbol  ex_date       applied on    held at ex-date?
//   ABT     2026-04-15    2026-04-18    no  -- first held 04-17, two days AFTER
//   AAPL    2026-05-11    2026-06-11    no  -- a month late
//   DD      2026-05-15    2026-06-12    no
//   TMUS    2026-05-29    2026-06-05    no
//   GOOGL   2026-06-08    2026-06-09    YES -- legitimate
//   META    2026-06-15    2026-06-16    YES -- legitimate
//
// The book was verifiably flat 2026-05-04..05-16 while runs were happening, so these are not
// missing-history artefacts. ABT alone moved the basis 95.47 -> 94.881429 and put +$33.79 of
// unrealized that does not exist onto the equity curve -- permanently, once the position sells,
// because the sale's realized is then struck against a fabricated basis.
//
// WHY IT HAPPENED. The applier walks CURRENTLY-held symbols and applies any event inside the
// fetch window [ex_date-14d, today]. A position opened after the ex-date was bought at a price
// the market had ALREADY adjusted, so restating it is a second, unearned adjustment. For a
// strategy that opens and closes names continuously this is ordinary, not exotic.
//
// The engine already detected it -- `qty_at_ex_date` was 0 and it logged a warning on exactly
// those four and on neither of the other two -- but the fallback only substituted today's
// quantity into the CASH figure. The old comment said so outright: "The basis adjustment is
// unaffected." That sentence was the bug.
//
// WHY THE FIX IS GATED. `qty_at_ex_date == 0` alone is ambiguous: it means either "genuinely
// flat" or "nothing is known about that date". Skipping on the second would DROP real
// adjustments, which is the worse failure. `book_state_known_at_ex_date` is the positive
// signal that the book state was actually observed.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;

namespace {

// Real ABT numbers from the failure.
constexpr double kQtyNow = 57.412048;
constexpr double kBasisPostEx = 95.47;        // close(2026-04-16), already ex-dividend
constexpr double kDividend = 0.63;
constexpr double kCloseAtEx = 101.56;         // close(2026-04-15)
constexpr const char* kExDate = "2026-04-15";

Position held(const std::string& sym, double qty, double avg) {
    Position p;
    p.symbol = sym;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg);
    return p;
}

CorpActionEvent dividend_event(double qty_at_ex, bool book_known) {
    CorpActionEvent ev;
    ev.symbol = "ABT";
    ev.ex_date = kExDate;
    ev.type = CorpActionType::DIVIDEND;
    ev.value = kDividend;
    ev.close_at_ex_date = kCloseAtEx;
    ev.qty_at_ex_date = qty_at_ex;
    ev.book_state_known_at_ex_date = book_known;
    return ev;
}

CorpActionEvent split_event(double qty_at_ex, bool book_known) {
    CorpActionEvent ev;
    ev.symbol = "BKNG";
    ev.ex_date = "2026-04-06";
    ev.type = CorpActionType::SPLIT;
    ev.value = 25.0;
    ev.qty_at_ex_date = qty_at_ex;
    ev.book_state_known_at_ex_date = book_known;
    return ev;
}

}  // namespace

// ---------------------------------------------------------------------------
// THE FIX. Verifiably flat at the ex-date -> the event must not touch the basis.
// ---------------------------------------------------------------------------

TEST(CorpActionExDateEligibility, VerifiablyFlatAtExDateSkipsTheEvent) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)}};

    auto adjustments =
        CorporateActionsApplier::apply(positions, {dividend_event(0.0, /*book_known=*/true)});

    EXPECT_TRUE(adjustments.empty())
        << "A dividend was applied to a position the book is on record as not holding at the "
           "ex-date. This is the ABT case: bought 04-17 at a price already ex-dividend.";

    EXPECT_DOUBLE_EQ(static_cast<double>(positions["ABT"].average_price), kBasisPostEx)
        << "The basis was restated for a dividend never received. Pre-fix this moved "
           "95.47 -> 94.881429, worth +$33.79 of unrealized that does not exist.";
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["ABT"].quantity), kQtyNow);
}

// The same rule must cover splits, where the magnitude is catastrophic rather than small.
TEST(CorpActionExDateEligibility, TheRuleCoversSplitsNotJustDividends) {
    std::unordered_map<std::string, Position> positions{
        {"BKNG", held("BKNG", 30.0, 167.7724)}};   // bought AFTER the split, post-split price

    auto adjustments =
        CorporateActionsApplier::apply(positions, {split_event(0.0, /*book_known=*/true)});

    EXPECT_TRUE(adjustments.empty());
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["BKNG"].quantity), 30.0)
        << "A 25:1 split was applied to a position bought after the ex-date at post-split "
           "prices. That multiplies a correct book by 25 -- the same class of error as F15, "
           "reached from the other direction.";
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["BKNG"].average_price), 167.7724);
}

// ---------------------------------------------------------------------------
// THE TWO WAYS THE FIX COULD BE WORSE THAN THE BUG.
// ---------------------------------------------------------------------------

// 1. A position genuinely held at the ex-date must still be adjusted. GOOGL and META in the
//    replay are this case, and they were correct before the fix and must stay correct.
TEST(CorpActionExDateEligibility, HeldAtExDateStillApplies) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)}};

    auto adjustments =
        CorporateActionsApplier::apply(positions, {dividend_event(kQtyNow, /*book_known=*/true)});

    ASSERT_EQ(adjustments.size(), 1u)
        << "A dividend on a position genuinely held at the ex-date was skipped. The fix must "
           "not drop real adjustments -- that is worse than the bug it replaces.";

    // 1e-6 not 1e-9: the applier rounds the restated basis, so the two differ at the 9th
    // decimal. That is a rounding artefact, not a basis error -- the defect this file exists
    // for moves the basis by 0.59, six orders of magnitude above this tolerance.
    const double ratio = 1.0 + kDividend / kCloseAtEx;
    EXPECT_NEAR(static_cast<double>(positions["ABT"].average_price), kBasisPostEx / ratio, 1e-6);
}

// 2. UNKNOWN must not be read as flat. A thin or absent position history is exactly when a
//    catch-up matters most, and silently skipping there would lose real adjustments with
//    nothing to indicate it. Unknown keeps the historical behaviour: apply, and warn.
TEST(CorpActionExDateEligibility, UnknownBookStateStillAppliesAndDoesNotSilentlyDrop) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)}};

    auto adjustments =
        CorporateActionsApplier::apply(positions, {dividend_event(0.0, /*book_known=*/false)});

    ASSERT_EQ(adjustments.size(), 1u)
        << "An unknown ex-date book state was treated as 'flat' and the event was dropped. "
           "Only a POSITIVE observation that the book held nothing may skip an event.";
}

// The default must be the safe one, so any caller that never sets the field keeps working.
TEST(CorpActionExDateEligibility, DefaultIsUnknownSoLegacyCallersAreUnaffected) {
    CorpActionEvent ev;
    EXPECT_FALSE(ev.book_state_known_at_ex_date)
        << "book_state_known_at_ex_date must default to false. A caller that does not populate "
           "it would otherwise start skipping every event for a symbol it holds.";
}
