#pragma once

#include "trade_ngin/portfolio/component_book.hpp"
#include "trade_ngin/portfolio/quantity_rule.hpp"

#include <map>
#include <vector>

namespace trade_ngin {

using InstrumentQuantityRules = std::map<InstrumentIdentity, QuantityRule>;

struct ComponentQuantityRounding {
    ComponentPositionKey key;
    InstrumentIdentity instrument;
    Quantity before;
    Quantity after;
};

struct ComponentQuantityNormalization {
    ComponentBookOverlay proposed_book;
    ComponentBookOverlay normalized_book;
    std::vector<ComponentQuantityRounding> roundings;
};

// Pure quantity-rule projection for a caller-supplied component book. This does
// not certify risk after rounding, approve or confirm a book, apply a risk
// recommendation scale, or determine who may receive recommendations.
Result<ComponentQuantityNormalization> normalize_component_quantities(
    const ComponentBookContext& context, const ComponentBookProposal& proposal,
    const InstrumentQuantityRules& rules);

}  // namespace trade_ngin
