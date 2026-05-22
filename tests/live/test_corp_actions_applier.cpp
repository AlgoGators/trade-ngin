#include <gtest/gtest.h>
#include <cmath>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"

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
    ev.close_t_minus_1 = close_tm1;
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
