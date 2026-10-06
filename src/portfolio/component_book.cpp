#include "trade_ngin/portfolio/component_book.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

namespace trade_ngin {
namespace {

bool has_text(const std::string& value) {
    return !value.empty() &&
           std::any_of(value.begin(), value.end(), [](unsigned char character) {
               return !std::isspace(character);
           });
}

bool valid_day(const std::string& day) {
    if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
    for (size_t i = 0; i < day.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (day[i] < '0' || day[i] > '9') return false;
    }
    const int year = (day[0] - '0') * 1000 + (day[1] - '0') * 100 +
                     (day[2] - '0') * 10 + (day[3] - '0');
    const int month = (day[5] - '0') * 10 + (day[6] - '0');
    const int date = (day[8] - '0') * 10 + (day[9] - '0');
    if (year == 0 || month < 1 || month > 12) return false;
    constexpr int days_per_month[] = {0, 31, 28, 31, 30, 31, 30, 31,
                                      31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int max_day = days_per_month[month] + (month == 2 && leap ? 1 : 0);
    return date >= 1 && date <= max_day;
}

bool valid_key(const ComponentPositionKey& key) {
    return has_text(key.portfolio_id) && has_text(key.strategy_id) &&
           has_text(key.strategy_name) && valid_day(key.date) &&
           has_text(key.symbol) && has_text(key.portfolio_type);
}

bool valid_asset_type(AssetType type) {
    switch (type) {
        case AssetType::FUTURE:
        case AssetType::EQUITY:
        case AssetType::OPTION:
        case AssetType::FOREX:
        case AssetType::CRYPTO:
            return true;
        case AssetType::NONE:
            return false;
    }
    return false;
}

bool can_add(int64_t lhs, int64_t rhs) {
    return (rhs <= 0 || lhs <= std::numeric_limits<int64_t>::max() - rhs) &&
           (rhs >= 0 || lhs >= std::numeric_limits<int64_t>::min() - rhs);
}

using OrderedSlots = std::map<ComponentPositionKey, const ComponentBookSlot*>;
using OrderedQuantities = std::map<ComponentPositionKey, Quantity>;

Result<void> validate_context(const ComponentBookContext& context,
                              OrderedSlots& slots) {
    if (!has_text(context.portfolio_id) || !valid_day(context.date) ||
        !has_text(context.portfolio_type) || !has_text(context.revision)) {
        return make_error<void>(ErrorCode::INVALID_DATA,
                                "invalid context identity", "component_book");
    }

    for (const auto& slot : context.slots) {
        if (!valid_key(slot.key) || slot.key.portfolio_id != context.portfolio_id ||
            slot.key.date != context.date ||
            slot.key.portfolio_type != context.portfolio_type ||
            !valid_asset_type(slot.instrument.type) ||
            !has_text(slot.instrument.symbol) ||
            slot.instrument.symbol != slot.key.symbol ||
            (slot.previous && slot.previous->symbol != slot.key.symbol)) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                    "inconsistent component slot", "component_book");
        }
        if (!slots.emplace(slot.key, &slot).second) {
            return make_error<void>(ErrorCode::INVALID_DATA,
                                    "duplicate component slot", "component_book");
        }
    }
    return Result<void>();
}

Result<void> validate_proposal(const ComponentBookContext& context,
                               const ComponentBookProposal& proposal,
                               const OrderedSlots& slots,
                               OrderedQuantities& quantities) {
    if (proposal.expected_portfolio_id != context.portfolio_id ||
        proposal.expected_date != context.date ||
        proposal.expected_portfolio_type != context.portfolio_type ||
        proposal.expected_revision != context.revision) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "stale or mismatched proposal context",
                                "component_book");
    }

    for (const auto& entry : proposal.quantities) {
        if (!valid_key(entry.key)) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "invalid proposal component key",
                                    "component_book");
        }
        const auto slot = slots.find(entry.key);
        if (slot == slots.end() || !slot->second->editable) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "proposal key outside editable scope",
                                    "component_book");
        }
        if (!quantities.emplace(entry.key, entry.quantity).second) {
            return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                    "duplicate proposal component key",
                                    "component_book");
        }
    }
    return Result<void>();
}

}  // namespace

Result<ComponentBookOverlay> overlay_component_book(
    const ComponentBookContext& context, const ComponentBookProposal& proposal) {
    using Overlay = ComponentBookOverlay;
    OrderedSlots slots;
    const auto context_validation = validate_context(context, slots);
    if (context_validation.is_error()) {
        const auto* error = context_validation.error();
        return make_error<Overlay>(error->code(), error->what(), error->component());
    }
    OrderedQuantities quantities;
    const auto proposal_validation =
        validate_proposal(context, proposal, slots, quantities);
    if (proposal_validation.is_error()) {
        const auto* error = proposal_validation.error();
        return make_error<Overlay>(error->code(), error->what(), error->component());
    }

    Overlay output{context.portfolio_id, context.date, context.portfolio_type,
                   context.revision, {}, {}};
    output.components.reserve(slots.size());
    std::map<InstrumentIdentity, InstrumentQuantityAggregate> aggregates;
    for (const auto& [key, slot] : slots) {
        Position position = slot->previous
                                ? *slot->previous
                                : Position(key.symbol, Quantity::from_raw(0),
                                           Price::from_raw(0), Decimal::from_raw(0),
                                           Decimal::from_raw(0), Timestamp{});
        if (slot->editable) {
            const auto proposal_entry = quantities.find(key);
            position.quantity = proposal_entry == quantities.end()
                                    ? Quantity::from_raw(0)
                                    : proposal_entry->second;
        }
        output.components.push_back({key, slot->instrument, slot->editable,
                                     slot->previous, position,
                                     !slot->previous.has_value()});

        auto [it, inserted] = aggregates.try_emplace(
            slot->instrument,
            InstrumentQuantityAggregate{slot->instrument, Quantity::from_raw(0), {}});
        (void)inserted;
        const int64_t current = it->second.net_quantity.raw_value();
        const int64_t added = position.quantity.raw_value();
        if (!can_add(current, added)) {
            return make_error<Overlay>(ErrorCode::INVALID_DATA,
                                       "typed instrument aggregate overflow",
                                       "component_book");
        }
        it->second.net_quantity = Quantity::from_raw(current + added);
        it->second.members.push_back(key);
    }
    output.instruments.reserve(aggregates.size());
    for (auto& [instrument, aggregate] : aggregates) {
        (void)instrument;
        output.instruments.push_back(std::move(aggregate));
    }
    return output;
}

}  // namespace trade_ngin
