#pragma once
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/portfolio/component_book.hpp"
#include "trade_ngin/live/corporate_actions_applier.hpp"
#include <optional>
#include <vector>

namespace trade_ngin {
// Pure owner-local arithmetic prerequisite; caller must admit source lineage,
// action eligibility and prices before composing any accounting/publication.
// Costs are separate, preserving pinned main's gross realized P&L semantics.
struct QtEquityPositionTransition {
    ComponentPositionKey key;
    Position restated_previous;
    Position position;
    Quantity quantity_change;
    Decimal gross_trade_realized_pnl;
    std::vector<PositionAdjustment> adjustments;
};

Result<QtEquityPositionTransition> produce_qt_equity_position_transition(
    const ComponentPositionKey& key, const std::optional<Position>& previous,
    Quantity selected, std::optional<double> reference_price,
    std::optional<double> mark_price, const Timestamp& timestamp,
    const std::vector<CorpActionEvent>& actions);
}
