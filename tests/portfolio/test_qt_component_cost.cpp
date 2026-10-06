#include "trade_ngin/portfolio/qt_component_cost.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace trade_ngin {
namespace {

ComponentPositionKey key(const char* owner) {
    return {"book", owner, owner, "2026-09-25", "XYZ", "qt_proposal"};
}

Position position(Quantity quantity) {
    return {"XYZ", quantity, Price(100), Decimal(0), Decimal(0), Timestamp{}};
}

ComponentBookOverlay selected(Quantity first, Quantity second,
                              Quantity prior_first = Quantity(5),
                              Quantity prior_second = Quantity(5)) {
    ComponentBookContext context{"book", "2026-09-25", "qt_proposal", "revision", {
        {key("alpha"), {AssetType::FUTURE, "XYZ"}, true, position(prior_first)},
        {key("beta"), {AssetType::FUTURE, "XYZ"}, true, position(prior_second)},
        {key("immutable"), {AssetType::FUTURE, "XYZ"}, false, position(Quantity(-3))},
    }};
    ComponentBookProposal proposal{"book", "2026-09-25", "qt_proposal", "revision", {
        {key("alpha"), first}, {key("beta"), second},
    }};
    auto overlay = overlay_component_book(context, proposal);
    EXPECT_TRUE(overlay.is_ok());
    return overlay.value();
}

std::vector<QtComponentCostInput> inputs() {
    return {
        {key("alpha"), {AssetType::FUTURE, "XYZ"}, Quantity(1), Quantity(0.25),
         "linear-cash-per-increment-v1", "synthetic-cost-a", "USD"},
        {key("beta"), {AssetType::FUTURE, "XYZ"}, Quantity(1), Quantity(0.25),
         "linear-cash-per-increment-v1", "synthetic-cost-b", "USD"},
    };
}

TEST(QtComponentCostTest, SelectedSplitChargesOnlyMovedOwner) {
    auto result = evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), inputs(), "USD", "selected-digest");
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    ASSERT_EQ(result.value().by_component.size(), 2U);
    EXPECT_EQ(result.value().by_component[0].key, key("alpha"));
    EXPECT_EQ(result.value().by_component[0].estimated_cash_cost.to_string(), "0");
    EXPECT_EQ(result.value().by_component[1].key, key("beta"));
    EXPECT_EQ(result.value().by_component[1].estimated_cash_cost.to_string(), "1");
    EXPECT_EQ(result.value().total_cash_cost.to_string(), "1");
    EXPECT_EQ(result.value().evaluated_book_digest, "selected-digest");

    auto alternate = evaluate_selected_component_costs(
        selected(Quantity(3), Quantity(3)), inputs(), "USD", "alternate-digest");
    ASSERT_TRUE(alternate.is_ok());
    EXPECT_EQ(alternate.value().by_component[0].estimated_cash_cost.to_string(), "0.5");
    EXPECT_EQ(alternate.value().by_component[1].estimated_cash_cost.to_string(), "0.5");
}

TEST(QtComponentCostTest, MissingOrUnsupportedSourceNeverProducesZeroEstimate) {
    auto costs = inputs();
    costs.pop_back();
    auto missing = evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest");
    ASSERT_TRUE(missing.is_error());
    EXPECT_NE(std::string(missing.error()->what()).find("cost_missing_input"), std::string::npos);

    costs = inputs();
    costs[1].source_id.clear();
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());
    costs[1].source_id = "synthetic-source";
    costs[1].approved_model_id = "unknown-nonlinear-model";
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());
}

TEST(QtComponentCostTest, RejectsCurrencyIncrementAndTypedKeyMismatch) {
    auto costs = inputs();
    costs[1].currency = "EUR";
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());
    costs = inputs();
    costs[1].calculation_increment = Quantity(3);
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());
    costs = inputs();
    costs[1].instrument.type = AssetType::EQUITY;
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());
    costs = inputs();
    costs.push_back(costs[1]);
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());
}

TEST(QtComponentCostTest, MissingInputCurrencyIsUnavailableDependency) {
    auto costs = inputs();
    costs[1].currency.clear();
    auto result = evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest");
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::DATA_NOT_FOUND);
    EXPECT_NE(std::string(result.error()->what()).find("cost_missing_currency"),
              std::string::npos);
}

TEST(QtComponentCostTest, ExactCashProductAndSumOverflowAreUnavailable) {
    auto costs = inputs();
    costs[1].cash_cost_per_increment = Quantity::from_raw(
        std::numeric_limits<int64_t>::max() / 3 + 1);
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(5), Quantity(1)), costs, "USD", "digest").is_error());

    costs = inputs();
    costs[0].cash_cost_per_increment = Quantity::from_raw(
        std::numeric_limits<int64_t>::max() / 2 + 1);
    costs[1].cash_cost_per_increment = costs[0].cash_cost_per_increment;
    EXPECT_TRUE(evaluate_selected_component_costs(
        selected(Quantity(4), Quantity(4)), costs, "USD", "digest").is_error());
}

TEST(QtComponentCostTest, SignedDeltaOverflowIsRejectedBeforeCostArithmetic) {
    auto book = selected(Quantity(5), Quantity(1));
    book.components[1].previous->quantity =
        Quantity::from_raw(std::numeric_limits<int64_t>::max());
    book.components[1].position.quantity =
        Quantity::from_raw(std::numeric_limits<int64_t>::min());
    auto result = evaluate_selected_component_costs(book, inputs(), "USD", "digest");
    ASSERT_TRUE(result.is_error());
    EXPECT_NE(std::string(result.error()->what()).find("cost_delta_overflow"),
              std::string::npos);
}

}  // namespace
}  // namespace trade_ngin
