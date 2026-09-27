#include "trade_ngin/portfolio/component_book.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace trade_ngin {
namespace {

ComponentPositionKey key(std::string strategy_id, std::string strategy_name,
                         std::string symbol = "ES.v.0",
                         std::string portfolio_id = "book-a",
                         std::string date = "2026-09-23",
                         std::string stream = "QT") {
    return {std::move(portfolio_id), std::move(strategy_id),
            std::move(strategy_name), std::move(date), std::move(symbol),
            std::move(stream)};
}

Position prior(const std::string& symbol, int64_t raw, int64_t basis = 12500000000,
               int64_t unrealized = 220000000, int64_t realized = -310000000) {
    return Position(symbol, Quantity::from_raw(raw), Price::from_raw(basis),
                    Decimal::from_raw(unrealized), Decimal::from_raw(realized),
                    Timestamp(std::chrono::seconds(111111)));
}

ComponentBookSlot slot(ComponentPositionKey identity, bool editable, int64_t raw,
                       AssetType type = AssetType::FUTURE) {
    const auto symbol = identity.symbol;
    return {std::move(identity), {type, symbol}, editable, prior(symbol, raw)};
}

ComponentBookContext context(std::vector<ComponentBookSlot> slots = {}) {
    return {"book-a", "2026-09-23", "QT", "revision-7", std::move(slots)};
}

ComponentBookProposal proposal(std::vector<ComponentQuantityEntry> entries = {}) {
    return {"book-a", "2026-09-23", "QT", "revision-7", std::move(entries)};
}

void expect_error(const ComponentBookContext& book, const ComponentBookProposal& edit,
                  ErrorCode code) {
    auto result = overlay_component_book(book, edit);
    ASSERT_TRUE(result.is_error());
    ASSERT_NE(result.error(), nullptr);
    EXPECT_EQ(result.error()->code(), code);
}

void expect_position_equal(const Position& actual, const Position& expected) {
    EXPECT_EQ(actual.symbol, expected.symbol);
    EXPECT_EQ(actual.quantity.raw_value(), expected.quantity.raw_value());
    EXPECT_EQ(actual.average_price.raw_value(), expected.average_price.raw_value());
    EXPECT_EQ(actual.unrealized_pnl.raw_value(), expected.unrealized_pnl.raw_value());
    EXPECT_EQ(actual.realized_pnl.raw_value(), expected.realized_pnl.raw_value());
    EXPECT_EQ(actual.last_update, expected.last_update);
}

void expect_slot_equal(const ComponentBookSlot& actual,
                       const ComponentBookSlot& expected) {
    EXPECT_EQ(actual.key, expected.key);
    EXPECT_EQ(actual.instrument, expected.instrument);
    EXPECT_EQ(actual.editable, expected.editable);
    ASSERT_EQ(actual.previous.has_value(), expected.previous.has_value());
    if (actual.previous) expect_position_equal(*actual.previous, *expected.previous);
}

void expect_context_equal(const ComponentBookContext& actual,
                          const ComponentBookContext& expected) {
    EXPECT_EQ(actual.portfolio_id, expected.portfolio_id);
    EXPECT_EQ(actual.date, expected.date);
    EXPECT_EQ(actual.portfolio_type, expected.portfolio_type);
    EXPECT_EQ(actual.revision, expected.revision);
    ASSERT_EQ(actual.slots.size(), expected.slots.size());
    for (size_t i = 0; i < actual.slots.size(); ++i) {
        expect_slot_equal(actual.slots[i], expected.slots[i]);
    }
}

void expect_proposal_equal(const ComponentBookProposal& actual,
                           const ComponentBookProposal& expected) {
    EXPECT_EQ(actual.expected_portfolio_id, expected.expected_portfolio_id);
    EXPECT_EQ(actual.expected_date, expected.expected_date);
    EXPECT_EQ(actual.expected_portfolio_type, expected.expected_portfolio_type);
    EXPECT_EQ(actual.expected_revision, expected.expected_revision);
    ASSERT_EQ(actual.quantities.size(), expected.quantities.size());
    for (size_t i = 0; i < actual.quantities.size(); ++i) {
        EXPECT_EQ(actual.quantities[i].key, expected.quantities[i].key);
        EXPECT_EQ(actual.quantities[i].quantity.raw_value(),
                  expected.quantities[i].quantity.raw_value());
    }
}

TEST(ComponentBookTest, SameSymbolComponentsKeepFullKeysAndPriorMetadata) {
    const auto first = key("engine-1", "shared-name");
    const auto second = key("engine-2", "shared-name");
    auto book = context({slot(second, false, 400000000), slot(first, true, 300000000)});
    auto edit = proposal({{first, Quantity::from_raw(500000001)}});
    const auto original_first = *book.slots[1].previous;
    const auto original_second = *book.slots[0].previous;

    auto result = overlay_component_book(book, edit);
    ASSERT_TRUE(result.is_ok());
    const auto& out = result.value();
    ASSERT_EQ(out.components.size(), 2u);
    EXPECT_EQ(out.components[0].key, first);
    EXPECT_EQ(out.components[1].key, second);
    EXPECT_EQ(out.components[0].position.quantity.raw_value(), 500000001);
    EXPECT_EQ(out.components[0].position.average_price.raw_value(), 12500000000);
    EXPECT_EQ(out.components[0].position.unrealized_pnl.raw_value(), 220000000);
    EXPECT_EQ(out.components[0].position.realized_pnl.raw_value(), -310000000);
    EXPECT_EQ(out.components[0].position.last_update, original_first.last_update);
    ASSERT_TRUE(out.components[0].previous.has_value());
    expect_position_equal(*out.components[0].previous, original_first);
    expect_position_equal(out.components[1].position, original_second);
    EXPECT_FALSE(out.components[0].unfilled);
    EXPECT_FALSE(out.components[1].unfilled);
    ASSERT_EQ(out.instruments.size(), 1u);
    EXPECT_EQ(out.instruments[0].instrument, (InstrumentIdentity{AssetType::FUTURE, "ES.v.0"}));
    EXPECT_EQ(out.instruments[0].net_quantity.raw_value(), 900000001);
    EXPECT_EQ(out.instruments[0].members, (std::vector<ComponentPositionKey>{first, second}));
    expect_position_equal(*book.slots[1].previous, original_first);
    expect_position_equal(*book.slots[0].previous, original_second);
    EXPECT_EQ(edit.quantities[0].quantity.raw_value(), 500000001);
}

TEST(ComponentBookTest, OmittedEditableHoldingExitsWhileImmutableAndEmptyProposalRetain) {
    const auto editable = key("a", "desk");
    const auto immutable = key("b", "other");
    const auto book = context({slot(editable, true, -700000000),
                               slot(immutable, false, 400000000)});
    auto result = overlay_component_book(book, proposal());
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().components.size(), 2u);
    EXPECT_EQ(result.value().components[0].position.quantity.raw_value(), 0);
    EXPECT_EQ(result.value().components[0].previous->quantity.raw_value(), -700000000);
    EXPECT_EQ(result.value().components[0].position.average_price.raw_value(), 12500000000);
    EXPECT_EQ(result.value().components[1].position.quantity.raw_value(), 400000000);
    EXPECT_EQ(result.value().instruments[0].net_quantity.raw_value(), 400000000);
    EXPECT_EQ(result.value().instruments[0].members.size(), 2u);
}

TEST(ComponentBookTest, OffsettingHoldingsRetainZeroNetAndMembers) {
    const auto a = key("a", "alpha");
    const auto b = key("b", "beta");
    auto result = overlay_component_book(context({slot(b, false, -100000000),
                                                  slot(a, true, 100000000)}),
                                         proposal({{a, Quantity::from_raw(100000000)}}));
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().instruments.size(), 1u);
    EXPECT_EQ(result.value().instruments[0].net_quantity.raw_value(), 0);
    EXPECT_EQ(result.value().instruments[0].members,
              (std::vector<ComponentPositionKey>{a, b}));
}

TEST(ComponentBookTest, DeclaredNewSlotIsUnfilledAndPriorZeroRemainsPrior) {
    const auto fresh = key("new", "desk", "AAPL");
    const auto old_zero = key("old", "desk", "ZERO");
    auto book = context({{fresh, {AssetType::EQUITY, "AAPL"}, true, std::nullopt},
                         slot(old_zero, true, 0, AssetType::EQUITY)});
    auto result = overlay_component_book(book,
                                         proposal({{fresh, Quantity::from_raw(1)}}));
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().components.size(), 2u);
    const auto& created = result.value().components[0];
    EXPECT_EQ(created.key, fresh);
    EXPECT_FALSE(created.previous.has_value());
    EXPECT_TRUE(created.unfilled);
    EXPECT_EQ(created.position.symbol, "AAPL");
    EXPECT_EQ(created.position.quantity.raw_value(), 1);
    EXPECT_EQ(created.position.average_price.raw_value(), 0);
    EXPECT_EQ(created.position.unrealized_pnl.raw_value(), 0);
    EXPECT_EQ(created.position.realized_pnl.raw_value(), 0);
    EXPECT_EQ(created.position.last_update, Timestamp{});
    const auto& zero = result.value().components[1];
    EXPECT_TRUE(zero.previous.has_value());
    EXPECT_FALSE(zero.unfilled);
    EXPECT_EQ(zero.previous->quantity.raw_value(), 0);

    auto omitted = overlay_component_book(book, proposal());
    ASSERT_TRUE(omitted.is_ok());
    EXPECT_TRUE(omitted.value().components[0].unfilled);
    EXPECT_EQ(omitted.value().components[0].position.quantity.raw_value(), 0);
}

TEST(ComponentBookTest, EmptyContextProducesEmptyCandidate) {
    auto result = overlay_component_book(context(), proposal());
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().components.empty());
    EXPECT_TRUE(result.value().instruments.empty());
    EXPECT_EQ(result.value().revision, "revision-7");
}

TEST(ComponentBookTest, TypedSymbolsAggregateSeparately) {
    auto future = key("a", "future", "ROOT");
    auto equity = key("b", "equity", "ROOT");
    auto result = overlay_component_book(context({slot(future, true, 100000000),
                                                  slot(equity, false, 300000000,
                                                       AssetType::EQUITY)}),
                                         proposal({{future, Quantity::from_raw(-200000000)}}));
    ASSERT_TRUE(result.is_ok());
    ASSERT_EQ(result.value().instruments.size(), 2u);
    EXPECT_EQ(result.value().instruments[0].instrument.type, AssetType::FUTURE);
    EXPECT_EQ(result.value().instruments[0].net_quantity.raw_value(), -200000000);
    EXPECT_EQ(result.value().instruments[1].instrument.type, AssetType::EQUITY);
    EXPECT_EQ(result.value().instruments[1].net_quantity.raw_value(), 300000000);
}

TEST(ComponentBookTest, RejectsRequestScopeAndStaleIdentity) {
    const auto a = key("a", "desk");
    auto book = context({slot(a, false, 100000000)});
    expect_error(book, proposal({{a, Quantity::from_raw(100000000)}}),
                 ErrorCode::INVALID_ARGUMENT);
    expect_error(book, proposal({{key("unknown", "desk"), Quantity(1)}}),
                 ErrorCode::INVALID_ARGUMENT);
    expect_error(book, proposal({{key("a", "desk", "UNDECLARED"), Quantity(1)}}),
                 ErrorCode::INVALID_ARGUMENT);
    expect_error(book, proposal({{a, Quantity(1)}, {a, Quantity(2)}}),
                 ErrorCode::INVALID_ARGUMENT);
    for (int field = 0; field < 4; ++field) {
        auto edit = proposal();
        if (field == 0) edit.expected_portfolio_id = "another";
        if (field == 1) edit.expected_date = "2026-09-22";
        if (field == 2) edit.expected_portfolio_type = "SYSTEM";
        if (field == 3) edit.expected_revision = "old";
        expect_error(book, edit, ErrorCode::INVALID_ARGUMENT);
    }
    expect_error(book, proposal({{key("a", "desk", "ES.v.0", "other"), Quantity(1)}}),
                 ErrorCode::INVALID_ARGUMENT);
}

TEST(ComponentBookTest, RejectsInconsistentContextIdentityAndCollisions) {
    const auto a = key("a", "desk");
    auto book = context({slot(a, true, 1), slot(a, true, 2, AssetType::EQUITY)});
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1), slot(a, false, 2)});
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.slots[0].instrument.symbol = "other";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book.slots[0].instrument.symbol = "  ";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.slots[0].previous->symbol = "other";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.slots[0].instrument.type = AssetType::NONE;
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book.slots[0].instrument.type = static_cast<AssetType>(99);
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(key("a", "desk", "ES.v.0", "another"), true, 1)});
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.slots[0].key.portfolio_type = "SYSTEM";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book.slots[0].key.portfolio_type = "QT";
    book.slots[0].key.date = "2026-09-22";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
}

TEST(ComponentBookTest, RejectsEmptyFieldsAndNoncanonicalCalendarDays) {
    const auto a = key("a", "desk");
    for (const auto& bad : {"", "  ", "2026-2-03", "2026-02-30", "2025-02-29",
                            "2026-13-01", "2026-00-01", "2026-01-00", "2026/01/01"}) {
        auto book = context({slot(a, true, 1)});
        book.slots[0].key.date = bad;
        expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    }
    auto leap = context({slot(key("a", "desk", "ES.v.0", "book-a", "2024-02-29"),
                               true, 1)});
    leap.date = "2024-02-29";
    auto leap_edit = proposal();
    leap_edit.expected_date = "2024-02-29";
    EXPECT_TRUE(overlay_component_book(leap, leap_edit).is_ok());
    auto book = context({slot(a, true, 1)});
    for (int field = 0; field < 5; ++field) {
        book = context({slot(a, true, 1)});
        if (field == 0) book.slots[0].key.portfolio_id = "\t ";
        if (field == 1) book.slots[0].key.strategy_id = "\t ";
        if (field == 2) book.slots[0].key.strategy_name = "\t ";
        if (field == 3) book.slots[0].key.symbol = "\t ";
        if (field == 4) book.slots[0].key.portfolio_type = "\t ";
        expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    }
    book = context({slot(a, true, 1)});
    book.revision = " ";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.portfolio_id = " ";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.portfolio_type = " ";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    book = context({slot(a, true, 1)});
    book.date = "2026-09-31";
    expect_error(book, proposal(), ErrorCode::INVALID_DATA);
    auto bad_edit = proposal({{key(" ", "desk"), Quantity(1)}});
    expect_error(context({slot(a, true, 1)}), bad_edit, ErrorCode::INVALID_ARGUMENT);
    auto empty_revision = proposal();
    empty_revision.expected_revision = " ";
    expect_error(context({slot(a, true, 1)}), empty_revision,
                 ErrorCode::INVALID_ARGUMENT);
}

TEST(ComponentBookTest, FractionalAndNegativeRawValuesRoundTrip) {
    auto a = key("a", "desk");
    auto b = key("b", "desk");
    auto result = overlay_component_book(
        context({slot(a, true, 100000001), slot(b, false, -100000000)}),
        proposal({{a, Quantity::from_raw(100000001)}}));
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().components[0].position.quantity.raw_value(), 100000001);
    EXPECT_EQ(result.value().components[1].position.quantity.raw_value(), -100000000);
    EXPECT_EQ(result.value().instruments[0].net_quantity.raw_value(), 1);
}

TEST(ComponentBookTest, RejectsPositiveAndNegativeAggregateOverflow) {
    const auto max = std::numeric_limits<int64_t>::max();
    const auto min = std::numeric_limits<int64_t>::min();
    auto a = key("a", "desk");
    auto b = key("b", "desk");
    expect_error(context({slot(a, false, max), slot(b, false, 1)}), proposal(),
                 ErrorCode::INVALID_DATA);
    expect_error(context({slot(a, false, min), slot(b, false, -1)}), proposal(),
                 ErrorCode::INVALID_DATA);
    auto c = key("c", "desk");
    expect_error(context({slot(a, false, max), slot(b, false, 1),
                          slot(c, false, -1)}), proposal(), ErrorCode::INVALID_DATA);
    auto boundary = overlay_component_book(context({slot(a, false, max),
                                                    slot(b, false, 0)}), proposal());
    ASSERT_TRUE(boundary.is_ok());
    EXPECT_EQ(boundary.value().instruments[0].net_quantity.raw_value(), max);
}

TEST(ComponentBookTest, PermutationsAndRepeatCallsYieldOrderedEqualValues) {
    const auto a = key("a", "shared");
    const auto b = key("b", "shared");
    const auto c = key("c", "other", "AAPL");
    auto book = context({slot(c, true, 300000001, AssetType::EQUITY),
                         slot(b, false, -200000000), slot(a, true, 100000000)});
    auto edit = proposal({{c, Quantity::from_raw(-300000001)},
                          {a, Quantity::from_raw(100000001)}});
    auto original_book = book;
    auto original_edit = edit;
    auto first = overlay_component_book(book, edit);
    ASSERT_TRUE(first.is_ok());
    expect_context_equal(book, original_book);
    expect_proposal_equal(edit, original_edit);
    std::reverse(book.slots.begin(), book.slots.end());
    std::reverse(edit.quantities.begin(), edit.quantities.end());
    const auto permuted_book = book;
    const auto permuted_edit = edit;
    for (int repeat = 0; repeat < 3; ++repeat) {
        auto next = overlay_component_book(book, edit);
        ASSERT_TRUE(next.is_ok());
        ASSERT_EQ(next.value().components.size(), first.value().components.size());
        ASSERT_EQ(next.value().instruments.size(), first.value().instruments.size());
        for (size_t i = 0; i < next.value().components.size(); ++i) {
            const auto& actual = next.value().components[i];
            const auto& expected = first.value().components[i];
            EXPECT_EQ(actual.key, expected.key);
            EXPECT_EQ(actual.instrument, expected.instrument);
            EXPECT_EQ(actual.editable, expected.editable);
            EXPECT_EQ(actual.unfilled, expected.unfilled);
            ASSERT_EQ(actual.previous.has_value(), expected.previous.has_value());
            if (actual.previous) {
                expect_position_equal(*actual.previous, *expected.previous);
            }
            expect_position_equal(actual.position, expected.position);
        }
        for (size_t i = 0; i < next.value().instruments.size(); ++i) {
            EXPECT_EQ(next.value().instruments[i].instrument,
                      first.value().instruments[i].instrument);
            EXPECT_EQ(next.value().instruments[i].net_quantity.raw_value(),
                      first.value().instruments[i].net_quantity.raw_value());
            EXPECT_EQ(next.value().instruments[i].members,
                      first.value().instruments[i].members);
        }
    }
    expect_context_equal(book, permuted_book);
    expect_proposal_equal(edit, permuted_edit);
}

}  // namespace
}  // namespace trade_ngin
