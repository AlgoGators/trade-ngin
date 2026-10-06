#include "trade_ngin/portfolio/component_quantity_normalization.hpp"

namespace trade_ngin {

Result<ComponentQuantityNormalization> normalize_component_quantities(
    const ComponentBookContext& context, const ComponentBookProposal& proposal,
    const InstrumentQuantityRules& rules) {
    using Output = ComponentQuantityNormalization;
    const auto submitted = overlay_component_book(context, proposal);
    if (submitted.is_error()) {
        const auto* error = submitted.error();
        return make_error<Output>(error->code(), error->what(), error->component());
    }

    ComponentBookProposal normalized_proposal{
        proposal.expected_portfolio_id, proposal.expected_date,
        proposal.expected_portfolio_type, proposal.expected_revision, {}};
    normalized_proposal.quantities.reserve(submitted.value().components.size());
    std::vector<ComponentQuantityRounding> roundings;
    for (const auto& component : submitted.value().components) {
        if (!component.editable) continue;

        const auto rule = rules.find(component.instrument);
        if (rule == rules.end()) {
            return make_error<Output>(ErrorCode::INVALID_ARGUMENT,
                                      "missing editable instrument quantity rule",
                                      "component_quantity_normalization");
        }
        const Quantity before = component.position.quantity;
        const auto normalized = normalize_quantity(before, rule->second);
        if (normalized.is_error()) {
            const auto* error = normalized.error();
            return make_error<Output>(error->code(), error->what(), error->component());
        }
        const Quantity after = normalized.value();
        normalized_proposal.quantities.push_back({component.key, after});
        if (before != after) {
            roundings.push_back({component.key, component.instrument, before, after});
        }
    }

    const auto rebuilt = overlay_component_book(context, normalized_proposal);
    if (rebuilt.is_error()) {
        const auto* error = rebuilt.error();
        return make_error<Output>(error->code(), error->what(), error->component());
    }
    return Output{submitted.value(), rebuilt.value(), std::move(roundings)};
}

}  // namespace trade_ngin
