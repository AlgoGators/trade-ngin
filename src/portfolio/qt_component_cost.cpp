#include "trade_ngin/portfolio/qt_component_cost.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>

namespace trade_ngin {
namespace {

constexpr std::string_view kLinearModel = "linear-cash-per-increment-v1";

bool has_text(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    });
}

Result<QtSelectedCostEvaluation> invalid(ErrorCode code, const char* reason) {
    return make_error<QtSelectedCostEvaluation>(code, reason, "qt_component_cost");
}

bool checked_delta(int64_t selected, int64_t previous, int64_t& delta) {
    if ((previous < 0 && selected > std::numeric_limits<int64_t>::max() + previous) ||
        (previous > 0 && selected < std::numeric_limits<int64_t>::min() + previous))
        return false;
    delta = selected - previous;
    return true;
}

uint64_t magnitude(int64_t value) {
    return value < 0 ? uint64_t{0} - static_cast<uint64_t>(value)
                     : static_cast<uint64_t>(value);
}

}  // namespace

Result<QtSelectedCostEvaluation> evaluate_selected_component_costs(
    const ComponentBookOverlay& selected,
    const std::vector<QtComponentCostInput>& inputs,
    std::string_view currency, std::string_view book_digest) {
    if (!has_text(currency) || !has_text(book_digest))
        return invalid(ErrorCode::DATA_NOT_FOUND, "cost_missing_currency_or_digest");

    std::map<ComponentPositionKey, const ComponentPositionCandidate*> components;
    for (const auto& component : selected.components) {
        if (!components.emplace(component.key, &component).second)
            return invalid(ErrorCode::INVALID_DATA, "cost_duplicate_component");
        if (!component.editable && component.previous &&
            component.position.quantity != component.previous->quantity)
            return invalid(ErrorCode::INVALID_DATA, "cost_immutable_quantity_changed");
    }

    std::map<ComponentPositionKey, const QtComponentCostInput*> by_key;
    for (const auto& input : inputs) {
        if (!by_key.emplace(input.key, &input).second)
            return invalid(ErrorCode::INVALID_ARGUMENT, "cost_duplicate_input");
        const auto component = components.find(input.key);
        if (component == components.end() || !component->second->editable)
            return invalid(ErrorCode::INVALID_ARGUMENT, "cost_input_outside_editable_scope");
    }

    QtSelectedCostEvaluation output{{}, Quantity::from_raw(0),
                                    std::string(currency), std::string(book_digest)};
    int64_t total_raw = 0;
    for (const auto& [key, component] : components) {
        if (!component->editable) continue;
        const auto source = by_key.find(key);
        if (source == by_key.end())
            return invalid(ErrorCode::DATA_NOT_FOUND, "cost_missing_input");
        const auto& input = *source->second;
        if (input.instrument != component->instrument)
            return invalid(ErrorCode::INVALID_ARGUMENT, "cost_instrument_mismatch");
        if (!has_text(input.source_id))
            return invalid(ErrorCode::DATA_NOT_FOUND, "cost_missing_source");
        if (input.approved_model_id != kLinearModel)
            return invalid(ErrorCode::DATA_NOT_FOUND, "cost_unsupported_model");
        if (!has_text(input.currency))
            return invalid(ErrorCode::DATA_NOT_FOUND, "cost_missing_currency");
        if (input.currency != currency)
            return invalid(ErrorCode::INVALID_ARGUMENT, "cost_currency_mismatch");
        const int64_t increment = input.calculation_increment.raw_value();
        const int64_t cash = input.cash_cost_per_increment.raw_value();
        if (increment <= 0 || cash < 0)
            return invalid(ErrorCode::INVALID_ARGUMENT, "cost_invalid_linear_terms");

        const Quantity previous = component->previous
            ? component->previous->quantity : Quantity::from_raw(0);
        const Quantity chosen = component->position.quantity;
        int64_t delta = 0;
        if (!checked_delta(chosen.raw_value(), previous.raw_value(), delta))
            return invalid(ErrorCode::INVALID_DATA, "cost_delta_overflow");
        const uint64_t count = magnitude(delta) / static_cast<uint64_t>(increment);
        if (magnitude(delta) % static_cast<uint64_t>(increment) != 0)
            return invalid(ErrorCode::INVALID_ARGUMENT, "cost_increment_mismatch");
        const auto max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (cash > 0 && count > max / static_cast<uint64_t>(cash))
            return invalid(ErrorCode::INVALID_DATA, "cost_product_overflow");
        const int64_t line_raw = static_cast<int64_t>(count * static_cast<uint64_t>(cash));
        if (line_raw > std::numeric_limits<int64_t>::max() - total_raw)
            return invalid(ErrorCode::INVALID_DATA, "cost_total_overflow");
        total_raw += line_raw;
        output.by_component.push_back({key, previous, chosen,
                                       Quantity::from_raw(line_raw), input.source_id});
    }
    output.total_cash_cost = Quantity::from_raw(total_raw);
    return output;
}

}  // namespace trade_ngin
