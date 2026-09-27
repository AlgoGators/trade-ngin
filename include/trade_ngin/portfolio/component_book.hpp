#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace trade_ngin {

// The six fields of trading.positions' stored primary key, in primary-key order.
struct ComponentPositionKey {
    std::string portfolio_id;
    std::string strategy_id;
    std::string strategy_name;
    std::string date;
    std::string symbol;
    std::string portfolio_type;

    bool operator==(const ComponentPositionKey&) const = default;
    bool operator<(const ComponentPositionKey& other) const {
        return std::tie(portfolio_id, strategy_id, strategy_name, date, symbol,
                        portfolio_type) <
               std::tie(other.portfolio_id, other.strategy_id, other.strategy_name,
                        other.date, other.symbol, other.portfolio_type);
    }
};

struct InstrumentIdentity {
    AssetType type{AssetType::NONE};
    std::string symbol;

    bool operator==(const InstrumentIdentity&) const = default;
    bool operator<(const InstrumentIdentity& other) const {
        return std::tie(type, symbol) < std::tie(other.type, other.symbol);
    }
};

struct ComponentBookSlot {
    ComponentPositionKey key;
    InstrumentIdentity instrument;
    bool editable{false};
    std::optional<Position> previous;
};

struct ComponentBookContext {
    std::string portfolio_id;
    std::string date;
    std::string portfolio_type;
    std::string revision;
    std::vector<ComponentBookSlot> slots;
};

struct ComponentQuantityEntry {
    ComponentPositionKey key;
    Quantity quantity;
};

struct ComponentBookProposal {
    std::string expected_portfolio_id;
    std::string expected_date;
    std::string expected_portfolio_type;
    std::string expected_revision;
    std::vector<ComponentQuantityEntry> quantities;
};

struct ComponentPositionCandidate {
    ComponentPositionKey key;
    InstrumentIdentity instrument;
    bool editable{false};
    std::optional<Position> previous;
    Position position;
    bool unfilled{false};
};

struct InstrumentQuantityAggregate {
    InstrumentIdentity instrument;
    Quantity net_quantity;
    std::vector<ComponentPositionKey> members;
};

struct ComponentBookOverlay {
    std::string portfolio_id;
    std::string date;
    std::string portfolio_type;
    std::string revision;
    std::vector<ComponentPositionCandidate> components;
    std::vector<InstrumentQuantityAggregate> instruments;
};

// Pure projection over a caller-supplied complete snapshot. No recommendation policy.
Result<ComponentBookOverlay> overlay_component_book(
    const ComponentBookContext& context, const ComponentBookProposal& proposal);

}  // namespace trade_ngin
