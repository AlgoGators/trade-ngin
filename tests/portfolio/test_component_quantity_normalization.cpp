#include "trade_ngin/portfolio/component_quantity_normalization.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace trade_ngin {
namespace {

using Mode = QuantityRoundingMode;

Quantity raw(int64_t value) { return Quantity::from_raw(value); }

ComponentPositionKey key(std::string strategy, std::string symbol = "SAME") {
    return {"book-a", std::move(strategy), "shared-name", "2026-09-23",
            std::move(symbol), "QT"};
}

Position prior(const std::string& symbol, int64_t quantity, int64_t basis,
               int64_t unrealized, int64_t realized, int64_t seconds) {
    return {symbol, raw(quantity), Price::from_raw(basis),
            Decimal::from_raw(unrealized), Decimal::from_raw(realized),
            Timestamp(std::chrono::seconds(seconds))};
}

ComponentBookSlot slot(ComponentPositionKey identity, AssetType type,
                       bool editable, std::optional<Position> previous) {
    const std::string symbol = identity.symbol;
    return {std::move(identity), {type, symbol}, editable, std::move(previous)};
}

ComponentBookContext context(std::vector<ComponentBookSlot> slots = {}) {
    return {"book-a", "2026-09-23", "QT", "revision-7", std::move(slots)};
}

ComponentBookProposal proposal(std::vector<ComponentQuantityEntry> entries = {}) {
    return {"book-a", "2026-09-23", "QT", "revision-7", std::move(entries)};
}

QuantityRule rule(int64_t step, Mode mode, int64_t minimum, int64_t maximum) {
    return {raw(step), mode, raw(minimum), raw(maximum)};
}

void expect_position(const Position& actual, const Position& expected,
                     int64_t quantity) {
    EXPECT_EQ(actual.symbol, expected.symbol);
    EXPECT_EQ(actual.quantity.raw_value(), quantity);
    EXPECT_EQ(actual.average_price.raw_value(), expected.average_price.raw_value());
    EXPECT_EQ(actual.unrealized_pnl.raw_value(), expected.unrealized_pnl.raw_value());
    EXPECT_EQ(actual.realized_pnl.raw_value(), expected.realized_pnl.raw_value());
    EXPECT_EQ(actual.last_update, expected.last_update);
}

void expect_candidate(const ComponentPositionCandidate& actual,
                      const ComponentPositionCandidate& expected) {
    EXPECT_EQ(actual.key, expected.key);
    EXPECT_EQ(actual.instrument, expected.instrument);
    EXPECT_EQ(actual.editable, expected.editable);
    EXPECT_EQ(actual.unfilled, expected.unfilled);
    ASSERT_EQ(actual.previous.has_value(), expected.previous.has_value());
    if (expected.previous) {
        expect_position(*actual.previous, *expected.previous,
                        expected.previous->quantity.raw_value());
    }
    expect_position(actual.position, expected.position,
                    expected.position.quantity.raw_value());
}

void expect_book(const ComponentBookOverlay& actual,
                 const ComponentBookOverlay& expected) {
    EXPECT_EQ(actual.portfolio_id, expected.portfolio_id);
    EXPECT_EQ(actual.date, expected.date);
    EXPECT_EQ(actual.portfolio_type, expected.portfolio_type);
    EXPECT_EQ(actual.revision, expected.revision);
    ASSERT_EQ(actual.components.size(), expected.components.size());
    for (size_t i = 0; i < expected.components.size(); ++i) {
        expect_candidate(actual.components[i], expected.components[i]);
    }
    ASSERT_EQ(actual.instruments.size(), expected.instruments.size());
    for (size_t i = 0; i < expected.instruments.size(); ++i) {
        EXPECT_EQ(actual.instruments[i].instrument, expected.instruments[i].instrument);
        EXPECT_EQ(actual.instruments[i].net_quantity.raw_value(),
                  expected.instruments[i].net_quantity.raw_value());
        EXPECT_EQ(actual.instruments[i].members, expected.instruments[i].members);
    }
}

void expect_inputs_unchanged(const ComponentBookContext& actual,
                             const ComponentBookContext& original,
                             const ComponentBookProposal& edit,
                             const ComponentBookProposal& original_edit,
                             const InstrumentQuantityRules& rules,
                             const InstrumentQuantityRules& original_rules) {
    EXPECT_EQ(actual.portfolio_id, original.portfolio_id);
    EXPECT_EQ(actual.date, original.date);
    EXPECT_EQ(actual.portfolio_type, original.portfolio_type);
    EXPECT_EQ(actual.revision, original.revision);
    ASSERT_EQ(actual.slots.size(), original.slots.size());
    for (size_t i = 0; i < original.slots.size(); ++i) {
        EXPECT_EQ(actual.slots[i].key, original.slots[i].key);
        EXPECT_EQ(actual.slots[i].instrument, original.slots[i].instrument);
        EXPECT_EQ(actual.slots[i].editable, original.slots[i].editable);
        ASSERT_EQ(actual.slots[i].previous.has_value(),
                  original.slots[i].previous.has_value());
        if (original.slots[i].previous) {
            expect_position(*actual.slots[i].previous, *original.slots[i].previous,
                            original.slots[i].previous->quantity.raw_value());
        }
    }
    EXPECT_EQ(edit.expected_portfolio_id, original_edit.expected_portfolio_id);
    EXPECT_EQ(edit.expected_date, original_edit.expected_date);
    EXPECT_EQ(edit.expected_portfolio_type, original_edit.expected_portfolio_type);
    EXPECT_EQ(edit.expected_revision, original_edit.expected_revision);
    ASSERT_EQ(edit.quantities.size(), original_edit.quantities.size());
    for (size_t i = 0; i < original_edit.quantities.size(); ++i) {
        EXPECT_EQ(edit.quantities[i].key, original_edit.quantities[i].key);
        EXPECT_EQ(edit.quantities[i].quantity.raw_value(),
                  original_edit.quantities[i].quantity.raw_value());
    }
    ASSERT_EQ(rules.size(), original_rules.size());
    auto original_it = original_rules.begin();
    for (const auto& [identity, quantity_rule] : rules) {
        EXPECT_EQ(identity, original_it->first);
        EXPECT_EQ(quantity_rule.increment.raw_value(),
                  original_it->second.increment.raw_value());
        EXPECT_EQ(quantity_rule.mode, original_it->second.mode);
        EXPECT_EQ(quantity_rule.minimum, original_it->second.minimum);
        EXPECT_EQ(quantity_rule.maximum, original_it->second.maximum);
        ++original_it;
    }
}

void expect_failure(const ComponentBookContext& book,
                    const ComponentBookProposal& edit,
                    const InstrumentQuantityRules& rules, ErrorCode code,
                    const std::string& component) {
    const auto original_book = book;
    const auto original_edit = edit;
    const auto original_rules = rules;
    const auto result = normalize_component_quantities(book, edit, rules);
    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), code);
    EXPECT_EQ(result.error()->component(), component);
    expect_inputs_unchanged(book, original_book, edit, original_edit, rules,
                            original_rules);
}

TEST(ComponentQuantityNormalizationTest,
     RoundsEditableComponentAndRetainsOffsettingImmutableMemberAndMetadata) {
    const auto editable = key("a");
    const auto immutable = key("b");
    const auto old_editable = prior("SAME", 77, 12345, 17, -19, 111);
    const auto old_immutable = prior("SAME", -100, 54321, -23, 29, 222);
    const auto book = context({slot(immutable, AssetType::EQUITY, false,
                                    old_immutable),
                               slot(editable, AssetType::EQUITY, true,
                                    old_editable)});
    const auto edit = proposal({{editable, raw(149)}});
    const InstrumentQuantityRules rules{{{AssetType::EQUITY, "SAME"},
                                         rule(100, Mode::toward_zero, -1000, 1000)}};
    const auto result = normalize_component_quantities(book, edit, rules);
    ASSERT_TRUE(result.is_ok()) << (result.error() ? result.error()->what() : "");
    const auto& value = result.value();
    ASSERT_EQ(value.proposed_book.components.size(), 2u);
    ASSERT_EQ(value.normalized_book.components.size(), 2u);
    EXPECT_EQ(value.proposed_book.components[0].key, editable);
    EXPECT_EQ(value.proposed_book.components[1].key, immutable);
    EXPECT_EQ(value.proposed_book.components[0].position.quantity.raw_value(), 149);
    EXPECT_EQ(value.normalized_book.components[0].position.quantity.raw_value(), 100);
    EXPECT_EQ(value.proposed_book.components[1].position.quantity.raw_value(), -100);
    expect_candidate(value.proposed_book.components[1],
                     value.normalized_book.components[1]);
    for (const auto* overlay : {&value.proposed_book, &value.normalized_book}) {
        const auto& a = overlay->components[0];
        const auto& b = overlay->components[1];
        EXPECT_EQ(a.key, editable);
        EXPECT_EQ(b.key, immutable);
        EXPECT_EQ(a.instrument, (InstrumentIdentity{AssetType::EQUITY, "SAME"}));
        EXPECT_EQ(b.instrument, (InstrumentIdentity{AssetType::EQUITY, "SAME"}));
        EXPECT_TRUE(a.editable);
        EXPECT_FALSE(b.editable);
        EXPECT_FALSE(a.unfilled);
        EXPECT_FALSE(b.unfilled);
        ASSERT_TRUE(a.previous.has_value());
        ASSERT_TRUE(b.previous.has_value());
        expect_position(*a.previous, old_editable, 77);
        expect_position(*b.previous, old_immutable, -100);
        expect_position(a.position, old_editable,
                        overlay == &value.proposed_book ? 149 : 100);
        expect_position(b.position, old_immutable, -100);
        ASSERT_EQ(overlay->instruments.size(), 1u);
        EXPECT_EQ(overlay->instruments[0].members,
                  (std::vector<ComponentPositionKey>{editable, immutable}));
    }
    EXPECT_EQ(value.proposed_book.instruments[0].net_quantity.raw_value(), 49);
    EXPECT_EQ(value.normalized_book.instruments[0].net_quantity.raw_value(), 0);
    ASSERT_EQ(value.roundings.size(), 1u);
    EXPECT_EQ(value.roundings[0].key, editable);
    EXPECT_EQ(value.roundings[0].instrument,
              (InstrumentIdentity{AssetType::EQUITY, "SAME"}));
    EXPECT_EQ(value.roundings[0].before.raw_value(), 149);
    EXPECT_EQ(value.roundings[0].after.raw_value(), 100);
    expect_inputs_unchanged(book, context({slot(immutable, AssetType::EQUITY,
                                                false, old_immutable),
                                           slot(editable, AssetType::EQUITY,
                                                true, old_editable)}),
                            edit, proposal({{editable, raw(149)}}), rules, rules);
}

TEST(ComponentQuantityNormalizationTest,
     SignedFractionalStepsAndNearestTiesProduceOrderedChanges) {
    const auto a = key("a", "ODD");
    const auto b = key("b", "TIE");
    const auto c = key("c", "TIE");
    const auto book = context({slot(c, AssetType::FUTURE, true, std::nullopt),
                               slot(b, AssetType::FUTURE, true, std::nullopt),
                               slot(a, AssetType::EQUITY, true, std::nullopt)});
    const auto edit = proposal({{c, raw(-35)}, {a, raw(-74)}, {b, raw(35)}});
    const InstrumentQuantityRules rules{
        {{AssetType::EQUITY, "ODD"}, rule(25, Mode::toward_zero, -100, 100)},
        {{AssetType::FUTURE, "TIE"},
         rule(10, Mode::nearest_ties_away_from_zero, -100, 100)}};
    const auto result = normalize_component_quantities(book, edit, rules);
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().roundings.size(), 3u);
    EXPECT_EQ(result.value().roundings[0].key, a);
    EXPECT_EQ(result.value().roundings[0].before.raw_value(), -74);
    EXPECT_EQ(result.value().roundings[0].after.raw_value(), -50);
    EXPECT_EQ(result.value().roundings[1].key, b);
    EXPECT_EQ(result.value().roundings[1].before.raw_value(), 35);
    EXPECT_EQ(result.value().roundings[1].after.raw_value(), 40);
    EXPECT_EQ(result.value().roundings[2].key, c);
    EXPECT_EQ(result.value().roundings[2].before.raw_value(), -35);
    EXPECT_EQ(result.value().roundings[2].after.raw_value(), -40);
    EXPECT_EQ(result.value().normalized_book.components[0].position.quantity.raw_value(), -50);
    EXPECT_EQ(result.value().normalized_book.components[1].position.quantity.raw_value(), 40);
    EXPECT_EQ(result.value().normalized_book.components[2].position.quantity.raw_value(), -40);
    EXPECT_EQ(result.value().normalized_book.instruments[0].net_quantity.raw_value(), 0);
    EXPECT_EQ(result.value().normalized_book.instruments[1].net_quantity.raw_value(), -50);
    EXPECT_EQ(result.value().normalized_book.instruments[0].members,
              (std::vector<ComponentPositionKey>{b, c}));
}

TEST(ComponentQuantityNormalizationTest, TypedSameSymbolRulesRemainSeparate) {
    const auto future = key("a", "ROOT");
    const auto equity = key("b", "ROOT");
    const auto book = context({slot(equity, AssetType::EQUITY, true, std::nullopt),
                               slot(future, AssetType::FUTURE, true, std::nullopt)});
    const auto edit = proposal({{equity, raw(149)}, {future, raw(149)}});
    const InstrumentQuantityRules rules{
        {{AssetType::FUTURE, "ROOT"}, rule(100, Mode::toward_zero, -500, 500)},
        {{AssetType::EQUITY, "ROOT"}, rule(25, Mode::toward_zero, -500, 500)}};
    const auto result = normalize_component_quantities(book, edit, rules);
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().normalized_book.instruments.size(), 2u);
    EXPECT_EQ(result.value().normalized_book.components[0].position.quantity.raw_value(), 100);
    EXPECT_EQ(result.value().normalized_book.components[1].position.quantity.raw_value(), 125);
    EXPECT_EQ(result.value().normalized_book.instruments[0].instrument,
              (InstrumentIdentity{AssetType::FUTURE, "ROOT"}));
    EXPECT_EQ(result.value().normalized_book.instruments[0].net_quantity.raw_value(), 100);
    EXPECT_EQ(result.value().normalized_book.instruments[0].members,
              (std::vector<ComponentPositionKey>{future}));
    EXPECT_EQ(result.value().normalized_book.instruments[1].instrument,
              (InstrumentIdentity{AssetType::EQUITY, "ROOT"}));
    EXPECT_EQ(result.value().normalized_book.instruments[1].net_quantity.raw_value(), 125);
    EXPECT_EQ(result.value().normalized_book.instruments[1].members,
              (std::vector<ComponentPositionKey>{equity}));
    ASSERT_EQ(result.value().roundings.size(), 2u);
    EXPECT_EQ(result.value().roundings[0].key, future);
    EXPECT_EQ(result.value().roundings[1].key, equity);
}

TEST(ComponentQuantityNormalizationTest,
     OmittedEditableExitsAndFreshPositionStaysUnfilledWithZeroBasis) {
    const auto omitted = key("a", "EXIT");
    const auto fresh = key("b", "NEW");
    const auto old = prior("EXIT", 375, 45678, 12, -8, 333);
    const auto book = context({slot(fresh, AssetType::EQUITY, true, std::nullopt),
                               slot(omitted, AssetType::FUTURE, true, old)});
    const auto edit = proposal({{fresh, raw(149)}});
    const InstrumentQuantityRules rules{
        {{AssetType::FUTURE, "EXIT"}, rule(25, Mode::reject_off_increment, -500, 500)},
        {{AssetType::EQUITY, "NEW"}, rule(100, Mode::toward_zero, -500, 500)}};
    const auto result = normalize_component_quantities(book, edit, rules);
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().normalized_book.components.size(), 2u);
    const auto& exit = result.value().normalized_book.components[0];
    const auto& created = result.value().normalized_book.components[1];
    EXPECT_EQ(exit.key, omitted);
    EXPECT_EQ(exit.position.quantity.raw_value(), 0);
    EXPECT_TRUE(exit.previous.has_value());
    EXPECT_FALSE(exit.unfilled);
    expect_position(exit.position, old, 0);
    EXPECT_EQ(created.key, fresh);
    EXPECT_EQ(created.position.quantity.raw_value(), 100);
    EXPECT_FALSE(created.previous.has_value());
    EXPECT_TRUE(created.unfilled);
    EXPECT_EQ(created.position.average_price.raw_value(), 0);
    EXPECT_EQ(created.position.unrealized_pnl.raw_value(), 0);
    EXPECT_EQ(created.position.realized_pnl.raw_value(), 0);
    EXPECT_EQ(created.position.last_update, Timestamp{});
    ASSERT_EQ(result.value().roundings.size(), 1u);
    EXPECT_EQ(result.value().roundings[0].key, fresh);
    EXPECT_EQ(result.value().proposed_book.instruments[0].net_quantity.raw_value(), 0);
    EXPECT_EQ(result.value().normalized_book.instruments[0].members,
              (std::vector<ComponentPositionKey>{omitted}));
}

TEST(ComponentQuantityNormalizationTest, EmptyAndImmutableOnlyBooksNeedNoRules) {
    const auto empty = normalize_component_quantities(context(), proposal(), {});
    ASSERT_TRUE(empty.is_ok());
    EXPECT_TRUE(empty.value().proposed_book.components.empty());
    EXPECT_TRUE(empty.value().normalized_book.components.empty());
    EXPECT_TRUE(empty.value().roundings.empty());

    const auto immutable = key("a");
    const auto old = prior("SAME", 149, 12345, 7, -4, 777);
    const auto book = context({slot(immutable, AssetType::EQUITY, false, old)});
    const InstrumentQuantityRules unused{
        {{AssetType::FUTURE, "UNUSED"}, QuantityRule{}}};
    const auto result = normalize_component_quantities(book, proposal(), unused);
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().normalized_book.components.size(), 1u);
    EXPECT_EQ(result.value().normalized_book.components[0].position.quantity.raw_value(), 149);
    EXPECT_EQ(result.value().normalized_book.instruments[0].net_quantity.raw_value(), 149);
    EXPECT_TRUE(result.value().roundings.empty());
    expect_candidate(result.value().normalized_book.components[0],
                     result.value().proposed_book.components[0]);
}

TEST(ComponentQuantityNormalizationTest,
     MissingInvalidBoundedAndRejectRulesFailWholeOperation) {
    const auto a = key("a", "A");
    const auto b = key("b", "B");
    const auto book = context({slot(a, AssetType::FUTURE, true, std::nullopt),
                               slot(b, AssetType::EQUITY, true, std::nullopt)});
    const auto edit = proposal({{a, raw(149)}, {b, raw(35)}});
    const auto good_a = rule(100, Mode::toward_zero, -500, 500);
    const auto good_b = rule(10, Mode::nearest_ties_away_from_zero, -500, 500);
    expect_failure(book, edit, {{{AssetType::FUTURE, "A"}, good_a}},
                   ErrorCode::INVALID_ARGUMENT, "component_quantity_normalization");
    expect_failure(book, proposal({{a, raw(149)}}),
                   {{{AssetType::FUTURE, "A"}, good_a}},
                   ErrorCode::INVALID_ARGUMENT, "component_quantity_normalization");
    expect_failure(book, edit,
                   {{{AssetType::FUTURE, "A"}, QuantityRule{}},
                    {{AssetType::EQUITY, "B"}, good_b}},
                   ErrorCode::INVALID_ARGUMENT, "quantity_rule");
    expect_failure(book, edit,
                   {{{AssetType::FUTURE, "A"}, rule(100, Mode::toward_zero, -100, 100)},
                    {{AssetType::EQUITY, "B"}, good_b}},
                   ErrorCode::INVALID_ARGUMENT, "quantity_rule");
    expect_failure(book, edit,
                   {{{AssetType::FUTURE, "A"}, good_a},
                    {{AssetType::EQUITY, "B"}, rule(40, Mode::toward_zero, 1, 40)}},
                   ErrorCode::INVALID_ARGUMENT, "quantity_rule");
    expect_failure(book, edit,
                   {{{AssetType::FUTURE, "A"}, good_a},
                    {{AssetType::EQUITY, "B"},
                     rule(10, Mode::reject_off_increment, -500, 500)}},
                   ErrorCode::INVALID_ARGUMENT, "quantity_rule");
}

TEST(ComponentQuantityNormalizationTest,
     OriginalOverlayValidationErrorsAndOverflowPropagate) {
    const auto a = key("a");
    const auto b = key("b");
    const auto book = context({slot(a, AssetType::FUTURE, true, std::nullopt)});
    const InstrumentQuantityRules rules{
        {{AssetType::FUTURE, "SAME"}, rule(1, Mode::reject_off_increment,
                                           std::numeric_limits<int64_t>::min(),
                                           std::numeric_limits<int64_t>::max())}};
    auto stale = proposal({{a, raw(2)}});
    stale.expected_revision = "old";
    expect_failure(book, stale, rules, ErrorCode::INVALID_ARGUMENT, "component_book");
    expect_failure(book, proposal({{b, raw(2)}}), rules,
                   ErrorCode::INVALID_ARGUMENT, "component_book");
    expect_failure(book, proposal({{a, raw(2)}, {a, raw(3)}}), rules,
                   ErrorCode::INVALID_ARGUMENT, "component_book");
    auto mismatch = book;
    mismatch.slots[0].instrument.symbol = "WRONG";
    expect_failure(mismatch, proposal({{a, raw(2)}}), rules,
                   ErrorCode::INVALID_DATA, "component_book");
    const auto overflow = context({slot(a, AssetType::FUTURE, true, std::nullopt),
                                   slot(b, AssetType::FUTURE, true, std::nullopt)});
    expect_failure(overflow,
                   proposal({{a, raw(std::numeric_limits<int64_t>::max())},
                             {b, raw(1)}}), rules,
                   ErrorCode::INVALID_DATA, "component_book");
}

TEST(ComponentQuantityNormalizationTest,
     ReaggregationOverflowAfterScalarNormalizationFailsWholeOperation) {
    constexpr auto high = std::numeric_limits<int64_t>::max();
    const auto a = key("a");
    const auto b = key("b");
    const auto book = context({slot(a, AssetType::FUTURE, true, std::nullopt),
                               slot(b, AssetType::FUTURE, true, std::nullopt)});
    const auto edit = proposal({{a, raw(9223372036854775804LL)}, {b, raw(3)}});
    const InstrumentQuantityRules rules{
        {{AssetType::FUTURE, "SAME"},
         rule(4, Mode::nearest_ties_away_from_zero,
              std::numeric_limits<int64_t>::min(), high)}};
    const auto original = overlay_component_book(book, edit);
    ASSERT_TRUE(original.is_ok());
    EXPECT_EQ(original.value().instruments[0].net_quantity.raw_value(), high);
    EXPECT_EQ(normalize_quantity(raw(9223372036854775804LL), rules.begin()->second)
                  .value().raw_value(), 9223372036854775804LL);
    EXPECT_EQ(normalize_quantity(raw(3), rules.begin()->second).value().raw_value(), 4);
    expect_failure(book, edit, rules, ErrorCode::INVALID_DATA, "component_book");
}

TEST(ComponentQuantityNormalizationTest,
     PermutationsRepeatsAndSecondPassYieldCanonicalIdempotentBooks) {
    const auto a = key("a", "SAME");
    const auto b = key("b", "SAME");
    const auto c = key("c", "NEW");
    auto book = context({slot(c, AssetType::EQUITY, true, std::nullopt),
                         slot(b, AssetType::FUTURE, false,
                              prior("SAME", -100, 123, 7, -8, 555)),
                         slot(a, AssetType::FUTURE, true,
                              prior("SAME", 77, 456, -9, 10, 666))});
    auto edit = proposal({{c, raw(149)}, {a, raw(149)}});
    const InstrumentQuantityRules rules{
        {{AssetType::FUTURE, "SAME"}, rule(100, Mode::toward_zero, -500, 500)},
        {{AssetType::EQUITY, "NEW"}, rule(25, Mode::toward_zero, -500, 500)}};
    const auto original_book = book;
    const auto original_edit = edit;
    const auto first = normalize_component_quantities(book, edit, rules);
    ASSERT_TRUE(first.is_ok());
    ASSERT_EQ(first.value().roundings.size(), 2u);
    EXPECT_EQ(first.value().roundings[0].key, a);
    EXPECT_EQ(first.value().roundings[1].key, c);
    expect_inputs_unchanged(book, original_book, edit, original_edit, rules, rules);
    std::reverse(book.slots.begin(), book.slots.end());
    std::reverse(edit.quantities.begin(), edit.quantities.end());
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto next = normalize_component_quantities(book, edit, rules);
        ASSERT_TRUE(next.is_ok());
        expect_book(next.value().proposed_book, first.value().proposed_book);
        expect_book(next.value().normalized_book, first.value().normalized_book);
        ASSERT_EQ(next.value().roundings.size(), first.value().roundings.size());
        for (size_t i = 0; i < next.value().roundings.size(); ++i) {
            EXPECT_EQ(next.value().roundings[i].key, first.value().roundings[i].key);
            EXPECT_EQ(next.value().roundings[i].instrument,
                      first.value().roundings[i].instrument);
            EXPECT_EQ(next.value().roundings[i].before.raw_value(),
                      first.value().roundings[i].before.raw_value());
            EXPECT_EQ(next.value().roundings[i].after.raw_value(),
                      first.value().roundings[i].after.raw_value());
        }
    }
    const auto permuted_book = book;
    const auto permuted_edit = edit;
    expect_inputs_unchanged(book, permuted_book, edit, permuted_edit, rules, rules);
    const auto second = normalize_component_quantities(
        book, proposal({{a, raw(100)}, {c, raw(125)}}), rules);
    ASSERT_TRUE(second.is_ok());
    expect_book(second.value().proposed_book, first.value().normalized_book);
    expect_book(second.value().normalized_book, first.value().normalized_book);
    EXPECT_TRUE(second.value().roundings.empty());
}

}  // namespace
}  // namespace trade_ngin
