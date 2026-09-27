#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/portfolio/component_book.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace trade_ngin {

struct QtDeskSelectedRow {
    ComponentPositionKey key;
    Quantity quantity;
};

struct QtDeskSelectedBook {
    std::vector<QtDeskSelectedRow> selected_rows;
    std::string selected_book_digest;
};

// Admit only the immutable A2 decision and its referenced preview. This pure
// gate does not claim current read-set freshness, trusted fills or publication.
Result<QtDeskSelectedBook> validate_qt_desk_decision_snapshot(
    const nlohmann::json& decision_row, const nlohmann::json& preview_row);

}  // namespace trade_ngin
