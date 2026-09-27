#pragma once

#include "trade_ngin/portfolio/component_book.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace trade_ngin {

struct QtComponentCostInput {
    ComponentPositionKey key;
    InstrumentIdentity instrument;
    Quantity calculation_increment;
    Quantity cash_cost_per_increment;
    std::string approved_model_id;
    std::string source_id;
    std::string currency;
};

struct QtComponentCostLine {
    ComponentPositionKey key;
    Quantity previous_quantity;
    Quantity selected_quantity;
    Quantity estimated_cash_cost;
    std::string source_id;
};

struct QtSelectedCostEvaluation {
    std::vector<QtComponentCostLine> by_component;
    Quantity total_cash_cost;
    std::string currency;
    std::string evaluated_book_digest;
};

Result<QtSelectedCostEvaluation> evaluate_selected_component_costs(
    const ComponentBookOverlay& selected,
    const std::vector<QtComponentCostInput>& inputs,
    std::string_view currency,
    std::string_view book_digest);

}  // namespace trade_ngin
