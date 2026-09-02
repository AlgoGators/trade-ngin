// E2-F17 regression pins — a corporate action must not restate a cost basis that was formed
// AFTER its ex-date.
//
// THE ORIGINAL FAILURE, measured over an 82-day live replay: FOUR of six dividends applied were
// applied to positions held on no part of their ex-date. ABT's 04-15 dividend restated a
// position first held on 04-17, moving the basis 95.47 -> 94.881429 and putting +$33.79 of
// unrealized that does not exist onto the equity curve -- permanently, once the position sells,
// because the sale's realized is then struck against a fabricated basis.
//
// THE FIRST FIX WAS WRONG IN TWO WAYS, and this file exists in its current form because of them:
//
//   1. It was INERT FOR SPLITS. The runner only populated the signal inside its DIVIDEND
//      branch, so splits and ADR splits always reached the applier with the default and the
//      guard could never fire -- leaving the catastrophic case (a 25:1 split restating a
//      post-ex-date basis by 2400%) unguarded while the dividend case looked covered.
//
//      *** THE PREVIOUS VERSION OF THIS FILE HAD A TEST NAMED "TheRuleCoversSplitsNotJustDividends"
//      *** AND IT PASSED THE WHOLE TIME. It constructed the event directly, so it proved the
//      *** APPLIER was type-agnostic -- which was true -- while production could never reach
//      *** that state. Every test below has the same blind spot BY CONSTRUCTION: they pin the
//      *** applier's RULE, and they cannot see whether the runner populates the input. Only the
//      *** live replay can. Do not read a green run here as coverage of the wiring.
//
//   2. Its cutoff date was OFF BY ONE, in the drop-a-real-adjustment direction. It measured the
//      book at `ex_date - 1`, which is the cash-eligibility cutoff for a DIVIDEND, not the
//      basis frame. A fill on run D is priced at close(D-1), so a position opened ON the
//      ex-date run has a PRE-event basis and must still be restated; the old rule called it
//      "flat at the ex-date" and skipped it.
//
// THE RULE NOW: contaminated <=> the basis was formed after the ex-date, carried as a tri-state
// so that "no evidence" can never be mistaken for "verifiably flat". Dropping a real 25:1 split
// leaves basis and mark 25x apart -- strictly worse than the double-adjustment being prevented
// -- so UNKNOWN must APPLY.

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;

namespace {

using Provenance = CorpActionEvent::BasisProvenance;

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

CorpActionEvent dividend_event(Provenance prov) {
    CorpActionEvent ev;
    ev.symbol = "ABT";
    ev.ex_date = kExDate;
    ev.type = CorpActionType::DIVIDEND;
    ev.value = kDividend;
    ev.close_at_ex_date = kCloseAtEx;
    ev.qty_at_ex_date = kQtyNow;   // cash-flow figure only; must NOT gate eligibility
    ev.basis_provenance = prov;
    return ev;
}

CorpActionEvent split_event(Provenance prov) {
    CorpActionEvent ev;
    ev.symbol = "BKNG";
    ev.ex_date = "2026-04-06";
    ev.type = CorpActionType::SPLIT;
    ev.value = 25.0;
    ev.basis_provenance = prov;
    return ev;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE FIX. Positive evidence of a post-ex-date basis -> refuse.
// ---------------------------------------------------------------------------

TEST(CorpActionExDateEligibility, BasisFormedAfterExDateSkipsTheEvent) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)}};

    auto adjustments = CorporateActionsApplier::apply(
        positions, {dividend_event(Provenance::FORMED_AFTER_EX_DATE)});

    EXPECT_TRUE(adjustments.empty())
        << "A dividend was applied to a basis formed after the ex-date. This is the ABT case: "
           "bought 04-17 at a price already ex-dividend.";
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["ABT"].average_price), kBasisPostEx)
        << "The basis was restated for a dividend never received. Pre-fix this moved "
           "95.47 -> 94.881429, worth +$33.79 of unrealized that does not exist.";
}

TEST(CorpActionExDateEligibility, TheApplierRuleIsTypeAgnostic) {
    // NOTE: this proves the APPLIER treats a split like a dividend. It does NOT prove the
    // runner populates provenance for splits -- the exact gap that shipped. See the header.
    std::unordered_map<std::string, Position> positions{
        {"BKNG", held("BKNG", 30.0, 167.7724)}};   // bought post-split, at post-split prices

    auto adjustments = CorporateActionsApplier::apply(
        positions, {split_event(Provenance::FORMED_AFTER_EX_DATE)});

    EXPECT_TRUE(adjustments.empty());
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["BKNG"].quantity), 30.0)
        << "A 25:1 split was applied to a position bought after the ex-date at post-split "
           "prices, multiplying a correct book by 25.";
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["BKNG"].average_price), 167.7724);
}

// ---------------------------------------------------------------------------
// 2. THE TWO WAYS THE FIX COULD BE WORSE THAN THE BUG. Both must hold.
// ---------------------------------------------------------------------------

// A basis formed on or before the ex-date is exactly what the event is FOR.
TEST(CorpActionExDateEligibility, BasisFormedOnOrBeforeExDateStillApplies) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)}};

    auto adjustments = CorporateActionsApplier::apply(
        positions, {dividend_event(Provenance::FORMED_ON_OR_BEFORE_EX_DATE)});

    ASSERT_EQ(adjustments.size(), 1u)
        << "A dividend on a legitimately pre-event basis was skipped. Dropping a real "
           "adjustment is worse than the bug this replaces.";

    // 1e-6 not 1e-9: the applier rounds the restated basis. The defect this file exists for
    // moves the basis by 0.59, six orders of magnitude above this tolerance.
    const double ratio = 1.0 + kDividend / kCloseAtEx;
    EXPECT_NEAR(static_cast<double>(positions["ABT"].average_price), kBasisPostEx / ratio, 1e-6);
}

// UNKNOWN must APPLY. This is the tri-state's entire reason for existing: the predecessor was a
// bool, and a bool cannot tell "observed flat" from "nothing known".
TEST(CorpActionExDateEligibility, UnknownProvenanceAppliesAndNeverSilentlyDrops) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)},
        {"BKNG", held("BKNG", 10.0, 4194.31)}};

    auto adjustments = CorporateActionsApplier::apply(
        positions, {dividend_event(Provenance::UNKNOWN), split_event(Provenance::UNKNOWN)});

    ASSERT_EQ(adjustments.size(), 2u)
        << "An unknown basis provenance was treated as contaminated and the events were "
           "dropped. Only POSITIVE evidence may skip -- a missing signal must apply, because "
           "a dropped 25:1 split leaves basis and mark 25x apart.";
    EXPECT_NEAR(static_cast<double>(positions["BKNG"].quantity), 250.0, 1e-9);
}

TEST(CorpActionExDateEligibility, DefaultProvenanceIsUnknownSoLegacyCallersApply) {
    CorpActionEvent ev;
    EXPECT_EQ(ev.basis_provenance, Provenance::UNKNOWN)
        << "The default must be UNKNOWN. A caller that does not populate provenance must keep "
           "applying events, never start skipping them.";
}

// The cash-flow figure must not be re-purposed as an eligibility signal -- conflating the two
// dates is precisely how the shipped guard came to skip positions it should have restated.
TEST(CorpActionExDateEligibility, QtyAtExDateDoesNotGateEligibility) {
    std::unordered_map<std::string, Position> positions{
        {"ABT", held("ABT", kQtyNow, kBasisPostEx)}};

    auto ev = dividend_event(Provenance::FORMED_ON_OR_BEFORE_EX_DATE);
    ev.qty_at_ex_date = 0.0;    // no holding on the register at ex_date-1 ...

    auto adjustments = CorporateActionsApplier::apply(positions, {ev});

    ASSERT_EQ(adjustments.size(), 1u)
        << "... but the basis is pre-event, so the event MUST still apply. A position opened ON "
           "the ex-date run is exactly this case: it is absent at ex_date-1 yet its fill was "
           "priced at close(ex_date-1), which is pre-event.";
}
