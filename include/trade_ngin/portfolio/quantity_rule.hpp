#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

#include <optional>

namespace trade_ngin {

enum class QuantityRoundingMode {
    unspecified,
    reject_off_increment,
    toward_zero,
    nearest_ties_away_from_zero,
};

// Every field is caller supplied. A default-constructed rule is invalid.
struct QuantityRule {
    Quantity increment;
    QuantityRoundingMode mode{QuantityRoundingMode::unspecified};
    std::optional<Quantity> minimum;
    std::optional<Quantity> maximum;
};

Result<Quantity> normalize_quantity(Quantity value, const QuantityRule& rule);

}  // namespace trade_ngin
