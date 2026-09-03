// tests/live/corp_actions/test_broker_frame.cpp
//
// F-8 -- the adjusted-frame <-> broker-basis rule (docs/BROKER_BASIS_RECONCILIATION.md).
//
// Pure arithmetic, no database. These pin the two statements the rule makes that a
// reconciliation cannot get wrong without either "fixing" a correct number or accepting a
// wrong one:
//
//   identity 2  B_broker == B_book x PRODUCT (1 + d_i/c_i)   -- exact, splits cancel
//   identity 3  the P&L gap is NOT zero, and its size is knowable
//
// The bare identity `ratio_change x avg_after == avg_before` already passes on the applier;
// it is a tripwire on one event, not this rule. What is new here is the CHAIN (many events,
// inverted from the ledger) and the fact that a missing factor is UNKNOWN rather than 1.0.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/live/broker_frame.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

using namespace trade_ngin;

namespace {

broker_frame::AppliedEvent dividend(const std::string& ex_date, double d, double c) {
    broker_frame::AppliedEvent ev;
    ev.symbol = "SYM";
    ev.ex_date = ex_date;
    ev.action_type = "DIVIDEND";
    ev.dividend_per_share = d;
    ev.ratio = broker_frame::dividend_basis_ratio(d, c);
    ev.ratio_known = true;
    return ev;
}

broker_frame::AppliedEvent split(const std::string& ex_date, double factor) {
    broker_frame::AppliedEvent ev;
    ev.symbol = "SYM";
    ev.ex_date = ex_date;
    ev.action_type = "SPLIT";
    ev.ratio = factor;
    ev.ratio_known = true;
    return ev;
}

}  // namespace

TEST(BrokerFrame, RawBasisRecoveredFromAdjustedBasisAndDividendChain) {
    // One dividend, worked by hand. Bought at the cum close 100.63; d = 0.63 against a
    // RAW ex-date close of 100.00, so the applier divides the basis by 1.0063.
    EXPECT_DOUBLE_EQ(broker_frame::dividend_basis_ratio(0.63, 100.0), 1.0063);

    const double b_broker = 100.63;
    const double b_book = b_broker / 1.0063;
    EXPECT_NEAR(b_book, 100.0, 1e-12);

    const std::vector<broker_frame::AppliedEvent> one = {dividend("2026-04-15", 0.63, 100.0)};
    EXPECT_NEAR(broker_frame::raw_basis(b_book, one), b_broker, 1e-6 * b_broker);

    // A CHAIN. Three dividends and a 4:1 split between the second and the third. The split
    // must contribute NOTHING -- both frames divide the basis by 4 -- so the answer is the
    // same whether it is in the list or not. Getting this wrong is off by the split factor,
    // which is the shape of E2-F15 seen from the reconciliation side.
    const double r1 = broker_frame::dividend_basis_ratio(0.63, 100.00);
    const double r2 = broker_frame::dividend_basis_ratio(0.71, 112.50);
    const double r3 = broker_frame::dividend_basis_ratio(0.24, 30.00);
    const double product = r1 * r2 * r3;

    const std::vector<broker_frame::AppliedEvent> chain = {
        dividend("2026-04-15", 0.63, 100.00),
        dividend("2026-07-15", 0.71, 112.50),
        split("2026-08-11", 4.0),
        dividend("2026-10-14", 0.24, 30.00),
    };
    const std::vector<broker_frame::AppliedEvent> without_split = {
        dividend("2026-04-15", 0.63, 100.00),
        dividend("2026-07-15", 0.71, 112.50),
        dividend("2026-10-14", 0.24, 30.00),
    };

    const double adjusted = 25.0;
    const double with_split = broker_frame::raw_basis(adjusted, chain);
    EXPECT_TRUE(broker_frame::basis_is_known(with_split));
    EXPECT_NEAR(with_split, adjusted * product, 1e-6 * adjusted * product);
    EXPECT_DOUBLE_EQ(with_split, broker_frame::raw_basis(adjusted, without_split))
        << "a split in the chain must not move the answer -- both frames divide by F";

    // Round trip: the broker basis divided back down by the same chain is the book basis.
    const double back = with_split / product;
    EXPECT_NEAR(back, adjusted, 1e-6 * adjusted);

    // No events at all: the two frames agree exactly. A position that never held through a
    // dividend needs no reconciliation footnote.
    EXPECT_DOUBLE_EQ(broker_frame::raw_basis(87.5, {}), 87.5);

    // NULL basis_ratio is UNKNOWN, not 1.0. A pre-migration-006 row anywhere in the chain
    // makes the inversion unanswerable, and the function must SAY so rather than return a
    // number that reads as broker-equivalent and is short by that event's factor.
    auto legacy = chain;
    legacy[1].ratio_known = false;
    EXPECT_FALSE(broker_frame::basis_is_known(broker_frame::raw_basis(adjusted, legacy)));
    EXPECT_DOUBLE_EQ(broker_frame::raw_basis(adjusted, legacy), broker_frame::basis_unknown());

    // ... but an unknown ratio on a SPLIT is harmless: splits are skipped anyway.
    auto legacy_split = chain;
    legacy_split[2].ratio_known = false;
    EXPECT_NEAR(broker_frame::raw_basis(adjusted, legacy_split), adjusted * product,
                1e-6 * adjusted * product);

    // Garbage in never becomes a plausible number out.
    EXPECT_FALSE(broker_frame::basis_is_known(broker_frame::raw_basis(0.0, one)));
    EXPECT_FALSE(broker_frame::basis_is_known(broker_frame::raw_basis(-5.0, one)));
    auto bad = one;
    bad[0].ratio = 0.0;
    EXPECT_FALSE(broker_frame::basis_is_known(broker_frame::raw_basis(adjusted, bad)));
    EXPECT_DOUBLE_EQ(broker_frame::dividend_basis_ratio(0.63, 0.0), 0.0);

    // The factor the ledger stores is exactly what the applier computed, so the chain read
    // back from trading.corp_action_applied inverts the basis the applier wrote.
    std::unordered_map<std::string, Position> positions;
    Position p;
    p.symbol = "SYM";
    p.quantity = Quantity(100.0);
    p.average_price = Decimal(100.63);
    positions["SYM"] = p;

    CorpActionEvent ce;
    ce.symbol = "SYM";
    ce.ex_date = "2026-04-15";
    ce.type = CorpActionType::DIVIDEND;
    ce.value = 0.63;
    ce.close_at_ex_date = 100.0;
    ce.basis_provenance = CorpActionEvent::BasisProvenance::FORMED_ON_OR_BEFORE_EX_DATE;

    auto adjustments = CorporateActionsApplier::apply(positions, {ce});
    ASSERT_EQ(adjustments.size(), 1u);
    EXPECT_DOUBLE_EQ(adjustments[0].ratio_change, 1.0063);

    broker_frame::AppliedEvent from_ledger;
    from_ledger.symbol = adjustments[0].symbol;
    from_ledger.ex_date = adjustments[0].event_date;
    from_ledger.action_type = CorporateActionsApplier::type_to_string(adjustments[0].type);
    from_ledger.ratio = adjustments[0].ratio_change;   // == basis_ratio on the dedup row
    from_ledger.dividend_per_share = adjustments[0].event_value;
    from_ledger.ratio_known = true;

    EXPECT_NEAR(broker_frame::raw_basis(positions["SYM"].average_price.as_double(),
                                        {from_ledger}),
                100.63, 1e-6 * 100.63);
}

TEST(BrokerFrame, PnLGapEqualsDividendTimesBasisDistance) {
    // Identity 3. The closed form for ONE dividend:
    //     U_book - (U_broker + D) = q * d * (B_broker - (c + d)) / (c + d)
    // Zero only when the position was bought at the cum-dividend close.
    const double q = 100.0;
    const double d = 0.63;
    const double c = 100.00;

    // Case 1 -- bought AT the cum close (100.63). The gap is zero.
    {
        const double b_broker = c + d;
        const double b_book = b_broker / broker_frame::dividend_basis_ratio(d, c);
        const std::vector<broker_frame::AppliedEvent> chain = {dividend("2026-04-15", d, c)};

        const double gap = broker_frame::expected_pnl_gap(q, b_broker, chain);
        EXPECT_NEAR(gap, 0.0, 1e-9);

        // ... and it agrees with the definition, evaluated at an arbitrary mark.
        const double M = 105.0;
        const double u_book = q * (M - b_book);
        const double u_broker_plus_cash = q * (M - b_broker) + q * d;
        EXPECT_NEAR(u_book - u_broker_plus_cash, gap, 1e-9);
    }

    // Case 2 -- bought well below the cum close. The gap is real, negative, and permanent.
    {
        const double b_broker = 90.0;
        const double b_book = b_broker / broker_frame::dividend_basis_ratio(d, c);
        const std::vector<broker_frame::AppliedEvent> chain = {dividend("2026-04-15", d, c)};

        const double closed_form = q * d * (b_broker - (c + d)) / (c + d);
        const double gap = broker_frame::expected_pnl_gap(q, b_broker, chain);
        EXPECT_NEAR(gap, closed_form, 1e-9);
        EXPECT_NEAR(gap, -6.654983, 1e-5);  // ~ -$6.65 on a $9,000 position

        const double M = 105.0;
        EXPECT_NEAR(q * (M - b_book) - (q * (M - b_broker) + q * d), gap, 1e-9);

        // The mark cancels: the gap is a property of the two BASES and the cash.
        const double M2 = 41.0;
        EXPECT_NEAR(q * (M2 - b_book) - (q * (M2 - b_broker) + q * d), gap, 1e-9);
    }

    // Case 3 -- a chain of two dividends and a split. The general form must still equal the
    // definition, and the split must again contribute nothing.
    {
        const double b_broker = 90.0;
        const std::vector<broker_frame::AppliedEvent> chain = {
            dividend("2026-04-15", 0.63, 100.00),
            split("2026-08-11", 4.0),
            dividend("2026-10-14", 0.24, 30.00),
        };
        double product = 1.0, cash = 0.0;
        for (const auto& ev : chain) {
            if (!broker_frame::is_dividend(ev)) continue;
            product *= ev.ratio;
            cash += ev.dividend_per_share;
        }
        const double b_book = b_broker / product;

        const double gap = broker_frame::expected_pnl_gap(q, b_broker, chain);
        const double M = 77.0;
        const double u_book = q * (M - b_book);
        const double u_broker_plus_cash = q * (M - b_broker) + q * cash;
        EXPECT_NEAR(u_book - u_broker_plus_cash, gap, 1e-9);
    }

    // A chain with no dividend has no gap -- splits alone leave the frames identical.
    EXPECT_DOUBLE_EQ(broker_frame::expected_pnl_gap(q, 90.0, {split("2026-08-11", 4.0)}), 0.0);
    EXPECT_DOUBLE_EQ(broker_frame::expected_pnl_gap(q, 90.0, {}), 0.0);

    // An unanswerable basis yields no claim about the gap, rather than a claim of zero
    // dressed up as a measurement -- raw_basis has already told the caller why.
    EXPECT_DOUBLE_EQ(
        broker_frame::expected_pnl_gap(q, broker_frame::basis_unknown(),
                                       {dividend("2026-04-15", d, c)}),
        0.0);

    // The stored ledger can imply the raw ex-date close it never stored: c = d/(r-1).
    EXPECT_NEAR(broker_frame::implied_close_at_ex_date(dividend("2026-04-15", d, c)), c,
                1e-9);
    EXPECT_DOUBLE_EQ(broker_frame::implied_close_at_ex_date(split("2026-08-11", 4.0)), 0.0);
}
