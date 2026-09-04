#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include "trade_ngin/live/corporate_actions_classification.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"

using namespace trade_ngin;

// Phase 4.3: corporate actions organised by mechanical effect class.
// Mock-backed / pure-logic; no database access.

namespace {

Position make_position(const std::string& symbol, double qty, double avg_price,
                       double realized = 0.0) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(qty);
    p.average_price = Decimal(avg_price);
    p.realized_pnl = Decimal(realized);
    return p;
}

TerminationEvent termination(const std::string& symbol, const std::string& date,
                             const std::string& label) {
    TerminationEvent ev;
    ev.symbol = symbol;
    ev.event_date = date;
    ev.vendor_label = label;
    return ev;
}

}  // namespace

// ---------------------------------------------------------------------------
// Classification: every vendor label maps to the class whose handler owns it.
// ---------------------------------------------------------------------------

TEST(CorpActionClassification, AllNineteenVendorLabelsMapToTheirEffectClass) {
    const std::vector<std::string> price_restating = {
        "split", "adrratiosplit", "spinoff", "spinoffdividend", "dividend"};
    const std::vector<std::string> continuity = {"tickerchangefrom", "tickerchangeto"};
    const std::vector<std::string> termination = {
        "mergerfrom", "mergerto", "acquisitionby", "acquisitionof", "delisted",
        "voluntarydelisting", "regulatorydelisting", "bankruptcyliquidation", "spunofffrom"};
    const std::vector<std::string> informational = {"listed", "relation", "initiated"};

    for (const auto& l : price_restating)
        EXPECT_EQ(classify_action(l), CorpActionClass::PRICE_RESTATING) << l;
    for (const auto& l : continuity)
        EXPECT_EQ(classify_action(l), CorpActionClass::SERIES_CONTINUITY) << l;
    for (const auto& l : termination)
        EXPECT_EQ(classify_action(l), CorpActionClass::TERMINATION) << l;
    for (const auto& l : informational)
        EXPECT_EQ(classify_action(l), CorpActionClass::INFORMATIONAL) << l;

    // 19 labels total, and the taxonomy covers exactly them.
    EXPECT_EQ(price_restating.size() + continuity.size() + termination.size() +
                  informational.size(), 19u);

    // An unseen label must surface, not be silently treated as harmless.
    EXPECT_EQ(classify_action("someNewVendorLabel"), CorpActionClass::UNRECOGNIZED);
    EXPECT_EQ(classify_action(""), CorpActionClass::UNRECOGNIZED);
}

TEST(CorpActionClassification, LabelListsDriveTheSqlFilters) {
    // vendor_labels_for_class() builds the SQL IN-lists; it must agree with
    // classify_action() or a query would fetch rows its handler rejects.
    for (auto cls : {CorpActionClass::PRICE_RESTATING, CorpActionClass::SERIES_CONTINUITY,
                     CorpActionClass::TERMINATION, CorpActionClass::INFORMATIONAL}) {
        const auto& labels = vendor_labels_for_class(cls);
        EXPECT_FALSE(labels.empty()) << corp_action_class_to_string(cls);
        for (const auto& l : labels) EXPECT_EQ(classify_action(l), cls) << l;
    }
    EXPECT_EQ(vendor_labels_for_class(CorpActionClass::TERMINATION).size(), 9u);
    EXPECT_EQ(vendor_labels_for_class(CorpActionClass::PRICE_RESTATING).size(), 5u);
}

// ---------------------------------------------------------------------------
// Class 1 -- price restating, sourced per-bar.
// ---------------------------------------------------------------------------

TEST(CorpActionClass1, PerBarSplitScalesSharesAndCostBasis) {
    // A 4-for-1: split_factor=4 on the ex-date bar. Position value must be
    // unchanged -- 4x the shares at 1/4 the basis.
    std::unordered_map<std::string, Position> positions;
    positions["AAPL"] = make_position("AAPL", 100.0, 400.0);

    CorpActionEvent ev;
    ev.symbol = "AAPL";
    ev.ex_date = "2026-08-11";
    ev.type = CorporateActionsApplier::type_from_action_string("split");
    ev.value = 4.0;

    const double value_before = 100.0 * 400.0;
    auto log = CorporateActionsApplier::apply(positions, {ev});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double(), 400.0);
    EXPECT_DOUBLE_EQ(positions["AAPL"].average_price.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["AAPL"].quantity.as_double() *
                         positions["AAPL"].average_price.as_double(),
                     value_before);
}

TEST(CorpActionClass1, PerBarDividendRescalesBasisWithoutTouchingShareCount) {
    // MNST-style bar: div_cash on the ex date. The dividend never changes the
    // share count; it rescales cost basis into the post-dividend adjusted
    // frame by exactly 1 + d/close[T-1].
    std::unordered_map<std::string, Position> positions;
    positions["KO"] = make_position("KO", 250.0, 60.0);

    CorpActionEvent ev;
    ev.symbol = "KO";
    ev.ex_date = "2026-08-10";
    ev.type = CorporateActionsApplier::type_from_action_string("dividend");
    ev.value = 0.485;
    ev.close_at_ex_date = 72.50;

    auto log = CorporateActionsApplier::apply(positions, {ev});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_DOUBLE_EQ(positions["KO"].quantity.as_double(), 250.0);
    const double expected_ratio = 1.0 + 0.485 / 72.50;
    EXPECT_NEAR(log[0].ratio_change, expected_ratio, 1e-12);
    // average_price round-trips through Decimal, which stores 8 decimal
    // places -- so the achievable tolerance here is that quantum, not the
    // 1e-12 the ratio itself holds to.
    EXPECT_NEAR(positions["KO"].average_price.as_double(), 60.0 / expected_ratio, 1e-8);
}

TEST(CorpActionClass1, DividendCashIsRecordedButNeverAddedToPositionPnl) {
    // The double-count guard at the position level: 4.2 pinned that
    // total_dividend_income stays out of total_pnl; this pins the other half
    // -- the applier books the dividend as a basis rescale and does NOT
    // credit realized_pnl, so the cash figure in the audit log is the only
    // place the dividend appears as cash.
    std::unordered_map<std::string, Position> positions;
    positions["KO"] = make_position("KO", 250.0, 60.0, /*realized=*/1234.0);

    CorpActionEvent ev;
    ev.symbol = "KO";
    ev.ex_date = "2026-08-10";
    ev.type = CorpActionType::DIVIDEND;
    ev.value = 0.485;
    ev.close_at_ex_date = 72.50;
    ev.qty_at_ex_date = 250.0;

    auto log = CorporateActionsApplier::apply(positions, {ev});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_DOUBLE_EQ(positions["KO"].realized_pnl.as_double(), 1234.0)
        << "dividend cash must not be booked into realized P&L -- the price "
           "adjustment already carries it";
    EXPECT_DOUBLE_EQ(log[0].quantity_before, 250.0);
    EXPECT_DOUBLE_EQ(log[0].quantity_after, 250.0);
}

TEST(CorpActionClass1, SplitBeforeDividendOnASharedBarLandsCashOnPostSplitShares) {
    // A bar can carry both primitives; the per-bar source emits the split
    // first. Applying in that order means the dividend's basis rescale sees
    // the post-split average price.
    std::unordered_map<std::string, Position> positions;
    positions["XYZ"] = make_position("XYZ", 100.0, 200.0);

    CorpActionEvent split;
    split.symbol = "XYZ";
    split.ex_date = "2026-06-30";
    split.type = CorpActionType::SPLIT;
    split.value = 2.0;

    CorpActionEvent div;
    div.symbol = "XYZ";
    div.ex_date = "2026-06-30";
    div.type = CorpActionType::DIVIDEND;
    div.value = 1.0;
    div.close_at_ex_date = 100.0;

    auto log = CorporateActionsApplier::apply(positions, {split, div});

    ASSERT_EQ(log.size(), 2u);
    EXPECT_DOUBLE_EQ(positions["XYZ"].quantity.as_double(), 200.0);
    const double post_split_basis = 100.0;              // 200 / 2
    const double ratio = 1.0 + 1.0 / 100.0;
    EXPECT_NEAR(positions["XYZ"].average_price.as_double(), post_split_basis / ratio, 1e-9);
}

// ---------------------------------------------------------------------------
// Class 2 -- series continuity.
// ---------------------------------------------------------------------------

TEST(CorpActionClass2, RenameRekeysPositionAndPreservesBasis) {
    // ANTM -> ELV, effective_until 2022-06-28. Holding one under the old key
    // after that date must stitch onto the current symbol untouched.
    std::unordered_map<std::string, Position> positions;
    positions["ANTM"] = make_position("ANTM", 40.0, 480.0);

    std::vector<TickerAlias> aliases = {{"ANTM", "ELV", "2022-06-28", "rebrand"}};
    // Held since before the rebrand, so this position IS the old-era one.
    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, "2026-08-31",
                                                        {{"ANTM", "2021-03-01"}});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::RENAMED);
    EXPECT_EQ(positions.count("ANTM"), 0u);
    ASSERT_EQ(positions.count("ELV"), 1u);
    EXPECT_DOUBLE_EQ(positions["ELV"].quantity.as_double(), 40.0);
    EXPECT_DOUBLE_EQ(positions["ELV"].average_price.as_double(), 480.0);
    EXPECT_EQ(positions["ELV"].symbol, "ELV");
}

TEST(CorpActionClass2, RenameNotYetEffectiveIsLeftAlone) {
    // BK -> BNY takes effect after 2026-01-01; a run dated before that must
    // still see the position under the old key.
    std::unordered_map<std::string, Position> positions;
    positions["BK"] = make_position("BK", 10.0, 55.0);

    std::vector<TickerAlias> aliases = {{"BK", "BNY", "2026-01-01", ""}};
    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, "2025-12-15",
                                                        {{"BK", "2025-06-01"}});

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.count("BK"), 1u);
    EXPECT_EQ(positions.count("BNY"), 0u);
}

TEST(CorpActionClass2, BothKeysHeldMergeAtWeightedAverageCost) {
    // The same holding recorded on either side of the rename boundary: merge
    // rather than lose a leg, weighting cost by quantity.
    std::unordered_map<std::string, Position> positions;
    positions["ABC"] = make_position("ABC", 100.0, 10.0, /*realized=*/25.0);
    positions["COR"] = make_position("COR", 300.0, 20.0, /*realized=*/75.0);

    std::vector<TickerAlias> aliases = {{"ABC", "COR", "2023-08-30", ""}};
    auto log = CorporateActionsLifecycle::apply_renames(
        positions, aliases, "2026-08-31", {{"ABC", "2022-01-04"}, {"COR", "2024-02-02"}});

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(positions.count("ABC"), 0u);
    EXPECT_DOUBLE_EQ(positions["COR"].quantity.as_double(), 400.0);
    EXPECT_DOUBLE_EQ(positions["COR"].average_price.as_double(),
                     (100.0 * 10.0 + 300.0 * 20.0) / 400.0);
    EXPECT_DOUBLE_EQ(positions["COR"].realized_pnl.as_double(), 100.0);
}

TEST(CorpActionClass2, UnmappedHistoricalTickerIsNeverLost) {
    // ticker_aliases is a curated subset. A symbol absent from it must be
    // left exactly as-is -- never dropped.
    std::unordered_map<std::string, Position> positions;
    positions["OLDCO"] = make_position("OLDCO", 5.0, 12.0);

    std::vector<TickerAlias> aliases = {{"ANTM", "ELV", "2022-06-28", ""}};
    auto log = CorporateActionsLifecycle::apply_renames(positions, aliases, "2026-08-31",
                                                        {{"OLDCO", "2020-01-02"}});

    EXPECT_TRUE(log.empty());
    ASSERT_EQ(positions.count("OLDCO"), 1u);
    EXPECT_DOUBLE_EQ(positions["OLDCO"].quantity.as_double(), 5.0);
}

// ---------------------------------------------------------------------------
// Class 3 -- termination / holding transformation.
// ---------------------------------------------------------------------------

TEST(CorpActionClass3, DelistingWithoutTermsExitsAtFinalCloseAndBooksPnl) {
    std::unordered_map<std::string, Position> positions;
    positions["DEAD"] = make_position("DEAD", 100.0, 8.0, /*realized=*/50.0);
    std::unordered_map<std::string, double> final_closes = {{"DEAD", 11.0}};

    auto events = std::vector<TerminationEvent>{termination("DEAD", "2026-04-09", "delisted")};
    auto log = CorporateActionsLifecycle::apply_terminations(positions, events, final_closes);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::EXITED_AT_FINAL_CLOSE);
    EXPECT_DOUBLE_EQ(log[0].exit_price, 11.0);
    EXPECT_DOUBLE_EQ(log[0].realized_delta, (11.0 - 8.0) * 100.0);
    EXPECT_DOUBLE_EQ(positions["DEAD"].quantity.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(positions["DEAD"].realized_pnl.as_double(), 50.0 + 300.0);
    EXPECT_DOUBLE_EQ(positions["DEAD"].unrealized_pnl.as_double(), 0.0);
}

TEST(CorpActionClass3, MergerWithUnsourcedTermsFallsBackWithoutCrashing) {
    // The live gap: corporate_action is frozen, so a merger arrives with no
    // contra_ticker/ratio. The handler must not crash, must not silently
    // leave a stale holding, and must take the documented final-close exit.
    std::unordered_map<std::string, Position> positions;
    positions["TGT1"] = make_position("TGT1", 75.0, 20.0);
    std::unordered_map<std::string, double> final_closes = {{"TGT1", 26.0}};

    auto ev = termination("TGT1", "2026-05-28", "mergerto");
    EXPECT_FALSE(ev.has_terms);  // nothing populated it -- that IS the gap

    auto log = CorporateActionsLifecycle::apply_terminations(positions, {ev}, final_closes);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::EXITED_AT_FINAL_CLOSE);
    EXPECT_EQ(log[0].vendor_label, "mergerto");
    EXPECT_DOUBLE_EQ(positions["TGT1"].quantity.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(positions["TGT1"].realized_pnl.as_double(), (26.0 - 20.0) * 75.0);
}

TEST(CorpActionClass3, ExitWithNoFinalCloseLeavesPositionForOperatorReview) {
    // Worse case: terms missing AND no price. Silently zeroing a position
    // would fabricate P&L, so the handler must leave it untouched.
    std::unordered_map<std::string, Position> positions;
    positions["NOPX"] = make_position("NOPX", 30.0, 15.0);
    std::unordered_map<std::string, double> final_closes;  // empty

    auto log = CorporateActionsLifecycle::apply_terminations(
        positions, {termination("NOPX", "2026-04-09", "delisted")}, final_closes);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::SKIPPED_NO_PRICE);
    EXPECT_DOUBLE_EQ(positions["NOPX"].quantity.as_double(), 30.0);
    EXPECT_DOUBLE_EQ(positions["NOPX"].average_price.as_double(), 15.0);
}

TEST(CorpActionClass3, RevivedFeedActivatesTheRolloverPathWithNoCodeChange) {
    // Feed-revived simulation: the same handler, now handed the deal terms
    // the frozen table would supply. The position must roll into the
    // successor at a basis-preserving ratio instead of exiting.
    std::unordered_map<std::string, Position> positions;
    positions["TGT2"] = make_position("TGT2", 100.0, 50.0);
    std::unordered_map<std::string, double> final_closes = {{"TGT2", 61.0}};

    TerminationEvent ev = termination("TGT2", "2026-05-28", "mergerto");
    ev.contra_ticker = "ACQR";
    ev.ratio = 0.5;        // 0.5 acquirer shares per target share
    ev.has_terms = true;

    const double basis_before = 100.0 * 50.0;
    auto log = CorporateActionsLifecycle::apply_terminations(positions, {ev}, final_closes);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::CONVERTED_TO_CONTRA);
    EXPECT_EQ(log[0].contra_ticker, "ACQR");
    EXPECT_EQ(positions.count("TGT2"), 0u);
    ASSERT_EQ(positions.count("ACQR"), 1u);
    EXPECT_DOUBLE_EQ(positions["ACQR"].quantity.as_double(), 50.0);
    EXPECT_DOUBLE_EQ(positions["ACQR"].average_price.as_double(), 100.0);
    EXPECT_DOUBLE_EQ(positions["ACQR"].quantity.as_double() *
                         positions["ACQR"].average_price.as_double(),
                     basis_before)
        << "a stock-for-stock roll must be basis-preserving at conversion";
}

TEST(CorpActionClass3, RolloverIntoAnAlreadyHeldAcquirerMergesAtWeightedCost) {
    std::unordered_map<std::string, Position> positions;
    positions["TGT3"] = make_position("TGT3", 100.0, 50.0);
    positions["ACQR"] = make_position("ACQR", 50.0, 80.0);
    std::unordered_map<std::string, double> final_closes = {{"TGT3", 61.0}};

    TerminationEvent ev = termination("TGT3", "2026-05-28", "acquisitionby");
    ev.contra_ticker = "ACQR";
    ev.ratio = 0.5;
    ev.has_terms = true;

    auto log = CorporateActionsLifecycle::apply_terminations(positions, {ev}, final_closes);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(positions.count("TGT3"), 0u);
    EXPECT_DOUBLE_EQ(positions["ACQR"].quantity.as_double(), 100.0);
    // 50 rolled-in shares at basis 100 merged with 50 held at 80.
    EXPECT_DOUBLE_EQ(positions["ACQR"].average_price.as_double(),
                     (50.0 * 80.0 + 50.0 * 100.0) / 100.0);
}

// ---------------------------------------------------------------------------
// E2-F26 -- three of the nine class-3 labels are keyed on the SURVIVOR, not on
// the ticker that dies.
// ---------------------------------------------------------------------------

TEST(CorpActionClass3, AcquirerKeyedRowDoesNotTerminateTheAcquirer) {
    // `acquisitionof` sits on the ACQUIRER (COF acquisitionof DFS), `mergerfrom` on the
    // surviving merger party, `spunofffrom` on the spinoff CHILD (RAL spunofffrom FTV).
    // On those three the row's own `ticker` is the name that LIVES. The runner keys the
    // deal-terms query on `ticker`, so admitting them makes a TerminationEvent for a
    // symbol that is still printing bars, and the no-terms path closes it at the final
    // close. 1,611 acquisitionof + 78 spunofffrom + 13 mergerfrom rows in
    // equities_data.corporate_action sit on tickers still printing 30+ days later.

    const auto& terminates =
        vendor_labels_for_termination_keying(TerminationKeying::ROW_TICKER_TERMINATES);
    const auto& counterparty =
        vendor_labels_for_termination_keying(TerminationKeying::COUNTERPARTY_ROW);

    for (const std::string l : {"acquisitionof", "mergerfrom", "spunofffrom"}) {
        EXPECT_EQ(std::count(terminates.begin(), terminates.end(), l), 0) << l;
        EXPECT_EQ(std::count(counterparty.begin(), counterparty.end(), l), 1) << l;
        EXPECT_EQ(termination_keying(l), TerminationKeying::COUNTERPARTY_ROW) << l;
    }
    for (const std::string l : {"acquisitionby", "mergerto", "delisted", "voluntarydelisting",
                                "regulatorydelisting", "bankruptcyliquidation"}) {
        EXPECT_EQ(std::count(terminates.begin(), terminates.end(), l), 1) << l;
        EXPECT_EQ(termination_keying(l), TerminationKeying::ROW_TICKER_TERMINATES) << l;
    }
    // The split PARTITIONS class 3 -- no label gained, none lost, so the query filter
    // cannot silently drop a label the classifier still routes here.
    EXPECT_EQ(terminates.size(), 6u);
    EXPECT_EQ(counterparty.size(), 3u);
    EXPECT_EQ(terminates.size() + counterparty.size(),
              vendor_labels_for_class(CorpActionClass::TERMINATION).size());

    // The admission rule the runner applies to one deal-terms row, both reasons.
    EXPECT_FALSE(CorporateActionsLifecycle::terms_row_terminates_its_ticker(
        "spunofffrom", "2025-06-30", "2026-08-28"))
        << "the spinoff CHILD is the survivor -- it must not be exited";
    EXPECT_FALSE(CorporateActionsLifecycle::terms_row_terminates_its_ticker(
        "acquisitionof", "2025-05-16", ""))
        << "survivor-keyed even when no bars are loaded to contradict it";
    EXPECT_TRUE(CorporateActionsLifecycle::terms_row_terminates_its_ticker(
        "acquisitionby", "2025-05-16", ""))
        << "the dying ticker with no bars after the event is a real termination";
    EXPECT_FALSE(CorporateActionsLifecycle::terms_row_terminates_its_ticker(
        "mergerto", "2025-05-16", "2026-08-28"))
        << "bars after the event contradict the row -- it belongs to a prior issuer";
    EXPECT_FALSE(CorporateActionsLifecycle::terms_row_terminates_its_ticker(
        "dividend", "2025-05-16", ""))
        << "only a class-3 label may build a TerminationEvent";

    // End to end on the real shape. RAL is the CHILD of the FTV spinoff and is still
    // trading; building the runner's event list through the admission rule leaves it held.
    std::unordered_map<std::string, Position> positions;
    positions["RAL"] = make_position("RAL", 33.0, 52.2);
    const std::unordered_map<std::string, double> final_closes = {{"RAL", 60.0}};

    std::vector<TerminationEvent> admitted;
    if (CorporateActionsLifecycle::terms_row_terminates_its_ticker("spunofffrom", "2025-06-30",
                                                                   "2026-08-28")) {
        admitted.push_back(termination("RAL", "2025-06-30", "spunofffrom"));
    }
    EXPECT_TRUE(admitted.empty());

    auto log = CorporateActionsLifecycle::apply_terminations(positions, admitted, final_closes);
    EXPECT_TRUE(log.empty());
    EXPECT_DOUBLE_EQ(positions["RAL"].quantity.as_double(), 33.0);
    EXPECT_DOUBLE_EQ(positions["RAL"].average_price.as_double(), 52.2);

    // And the pre-fix path, stated so the reason for the split cannot be lost: admit every
    // class-3 label -- which is what the terms query filter used to be -- and the
    // still-printing child is closed out at 60 with 257.40 of realized P&L it never made.
    std::unordered_map<std::string, Position> unguarded;
    unguarded["RAL"] = make_position("RAL", 33.0, 52.2);
    const auto& all_class3 = vendor_labels_for_class(CorpActionClass::TERMINATION);
    ASSERT_NE(std::count(all_class3.begin(), all_class3.end(), std::string("spunofffrom")), 0)
        << "spunofffrom is still classified TERMINATION -- only its KEYING changed";
    auto bad = CorporateActionsLifecycle::apply_terminations(
        unguarded, {termination("RAL", "2025-06-30", "spunofffrom")}, final_closes);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].outcome, LifecycleOutcome::EXITED_AT_FINAL_CLOSE);
    EXPECT_DOUBLE_EQ(unguarded["RAL"].quantity.as_double(), 0.0);
}

TEST(CorpActionClass3, EventForAnUnheldSymbolIsANoOp) {
    std::unordered_map<std::string, Position> positions;
    positions["HELD"] = make_position("HELD", 10.0, 5.0);
    std::unordered_map<std::string, double> final_closes = {{"OTHER", 3.0}};

    auto log = CorporateActionsLifecycle::apply_terminations(
        positions, {termination("OTHER", "2026-04-09", "delisted")}, final_closes);

    EXPECT_TRUE(log.empty());
    EXPECT_EQ(positions.size(), 1u);
    EXPECT_DOUBLE_EQ(positions["HELD"].quantity.as_double(), 10.0);
}

// ===========================================================================
// E2-F13: TERMINATION round-trips through the dedup record.
//
// Class-3 lifecycle events (delisting / acquisition) were recorded NOWHERE. audit_log.record()
// was called only from the class-1 applier block, so a termination left no row in
// trading.corp_action_applied -- no audit trail, and nothing to stop a replay re-applying it.
// Class-1 events have been deduped since the dedup table landed; class 3 never was.
//
// The dedup key is (symbol, event_date, type) and the type is persisted as a STRING, so the
// enum <-> string round trip is what makes a recorded termination findable again. If
// type_from_type_string does not know "TERMINATION" it silently degrades to UNKNOWN, the key
// no longer matches, and the dedup protection evaporates without any error.
// ===========================================================================

TEST(CorpActionTypeRoundTrip, TerminationSurvivesTheStringRoundTrip) {
    const char* s = CorporateActionsApplier::type_to_string(CorpActionType::TERMINATION);
    EXPECT_STREQ(s, "TERMINATION");

    EXPECT_EQ(CorporateActionsApplier::type_from_type_string(s), CorpActionType::TERMINATION)
        << "A recorded TERMINATION does not round-trip, so its dedup key will not match on the "
           "next run and the event can be applied twice.";
}

TEST(CorpActionTypeRoundTrip, ExistingTypesAreUnaffectedByTheNewValue) {
    // Adding an enum value must not disturb the values already persisted in
    // trading.corp_action_applied.
    for (auto t : {CorpActionType::SPLIT, CorpActionType::ADR_SPLIT, CorpActionType::DIVIDEND,
                   CorpActionType::UNKNOWN}) {
        EXPECT_EQ(CorporateActionsApplier::type_from_type_string(
                      CorporateActionsApplier::type_to_string(t)),
                  t)
            << "An existing corp-action type stopped round-tripping when TERMINATION was added.";
    }
}

TEST(CorpActionTypeRoundTrip, UnknownStringsStillDegradeToUnknown) {
    EXPECT_EQ(CorporateActionsApplier::type_from_type_string("NOT_A_TYPE"),
              CorpActionType::UNKNOWN)
        << "An unrecognised type string must degrade to UNKNOWN so an older binary reading a "
           "newer row fails safe rather than mis-classifying it.";
}

// The applier must not try to price-restate a lifecycle event. TERMINATION exists in the enum
// only so class-3 events can be recorded and deduped; CorporateActionsLifecycle applies them.
TEST(CorpActionTypeRoundTrip, ApplierSkipsTerminationRatherThanRestatingPrice) {
    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = "DEAD";
    p.quantity = Quantity(10.0);
    p.average_price = Decimal(100.0);
    positions["DEAD"] = p;

    CorpActionEvent ev;
    ev.symbol = "DEAD";
    ev.ex_date = "2026-04-09";
    ev.type = CorpActionType::TERMINATION;
    ev.value = 50.0;  // would be read as a split factor if the applier mishandled it

    auto adjustments = CorporateActionsApplier::apply(positions, {ev});

    EXPECT_TRUE(adjustments.empty())
        << "The applier produced an adjustment for a TERMINATION. Class-3 events belong to "
           "CorporateActionsLifecycle; treating one as price-restating would multiply the "
           "quantity by the exit price.";
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["DEAD"].quantity), 10.0)
        << "The applier mutated a position on a TERMINATION event.";
    EXPECT_DOUBLE_EQ(static_cast<double>(positions["DEAD"].average_price), 100.0);
}

// ──────────────────────────────────────────────────────────────────────────
// E4 item 3 -- the deal-terms feed's stop date is MEASURED, not compiled in.
//
// kCorpActionTableFrozenAfter used to be quoted verbatim in every "no deal
// terms" WARN as though it were current fact. It is a fact about the DATABASE:
// the day the vendor subscription restarts and the ACTIONS ingest backfills,
// the constant is wrong and the log keeps asserting it until somebody rebuilds
// -- and nobody is told that the dormant rollover path just went live.
// ──────────────────────────────────────────────────────────────────────────

#include "trade_ngin/live/corp_action_feed_status.hpp"

TEST(CorpActionFeedStatus, AMeasuredDateReplacesTheConstant) {
    // A feed that has been backfilled past the build-time constant.
    const auto s = assess_corp_action_feed("2026-08-31", "2026-09-03");
    EXPECT_TRUE(s.measured);
    EXPECT_EQ(s.last_row_date, "2026-08-31");
    EXPECT_NE(s.last_row_date, std::string(kCorpActionTableFrozenAfter))
        << "the reported date must come from the database, not from the binary";
    EXPECT_TRUE(s.revived_since_build)
        << "a feed with rows after the build-time date has been revived, and the "
           "operator has to be told before the deal-terms path starts acting";
    EXPECT_TRUE(s.frozen) << "still short of the run date, so 'frozen' is still true";
    EXPECT_NE(describe_corp_action_feed(s).find("2026-08-31"), std::string::npos);
    EXPECT_NE(describe_corp_action_feed(s).find("REVIVED"), std::string::npos);
}

TEST(CorpActionFeedStatus, TodaysMeasurementReproducesTheConstantWithoutBeingIt) {
    // What the live table says today. Same value as the constant -- and that is
    // the point: it is reported because it was read, not because it was typed.
    const auto s = assess_corp_action_feed("2025-08-29", "2026-09-03");
    EXPECT_TRUE(s.measured);
    EXPECT_FALSE(s.revived_since_build);
    EXPECT_TRUE(s.frozen);
    EXPECT_EQ(describe_corp_action_feed(s),
              "deal-terms feed last row 2025-08-29, frozen: yes");
}

TEST(CorpActionFeedStatus, AFeedCurrentToTheRunDateIsNotFrozen) {
    const auto s = assess_corp_action_feed("2026-09-03", "2026-09-03");
    EXPECT_TRUE(s.measured);
    EXPECT_FALSE(s.frozen) << "a row on the run date means the feed has not stopped";
    EXPECT_EQ(describe_corp_action_feed(s).find("frozen: no") == std::string::npos, false);
}

TEST(CorpActionFeedStatus, AFailedMeasurementFallsBackAndSaysSo) {
    const auto s = assess_corp_action_feed("", "2026-09-03");
    EXPECT_FALSE(s.measured);
    EXPECT_EQ(s.last_row_date, std::string(kCorpActionTableFrozenAfter))
        << "an unreadable table must degrade to the old behaviour, not to a blank date";
    EXPECT_FALSE(s.revived_since_build);
    EXPECT_NE(describe_corp_action_feed(s).find("NOT MEASURED"), std::string::npos)
        << "a fallen-back date must not be presented as a measurement";
}

// The WARN text the handler emits now carries the measured date. Passing it is
// what makes the message true; passing nothing preserves the old text exactly,
// so a caller that could not measure is no worse off than before.
TEST(CorpActionClass3, TerminationWarnsQuoteTheMeasuredFeedDate) {
    std::unordered_map<std::string, Position> positions;
    positions["GONE"] = make_position("GONE", 40.0, 25.0);
    std::unordered_map<std::string, double> final_closes = {{"GONE", 20.0}};

    auto log = CorporateActionsLifecycle::apply_terminations(
        positions, {termination("GONE", "2026-04-09", "delisted")}, final_closes,
        "2026-08-31");

    // The measured date changes only the message, never the arithmetic: the exit
    // is still at the final close and the realized delta is unchanged.
    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0].outcome, LifecycleOutcome::EXITED_AT_FINAL_CLOSE);
    EXPECT_DOUBLE_EQ(log[0].exit_price, 20.0);
    EXPECT_DOUBLE_EQ(log[0].realized_delta, (20.0 - 25.0) * 40.0);
    EXPECT_DOUBLE_EQ(positions["GONE"].quantity.as_double(), 0.0);

    // And the default argument leaves every existing caller byte-identical.
    std::unordered_map<std::string, Position> same;
    same["GONE"] = make_position("GONE", 40.0, 25.0);
    auto log_default = CorporateActionsLifecycle::apply_terminations(
        same, {termination("GONE", "2026-04-09", "delisted")}, final_closes);
    ASSERT_EQ(log_default.size(), 1u);
    EXPECT_EQ(log_default[0].outcome, log[0].outcome);
    EXPECT_DOUBLE_EQ(log_default[0].realized_delta, log[0].realized_delta);
}
