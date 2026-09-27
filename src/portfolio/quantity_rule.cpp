#include "trade_ngin/portfolio/quantity_rule.hpp"

#include <cstdint>
#include <limits>

namespace trade_ngin {

Result<Quantity> normalize_quantity(Quantity value, const QuantityRule& rule) {
    const auto invalid = [](const char* reason) {
        return make_error<Quantity>(ErrorCode::INVALID_ARGUMENT, reason,
                                    "quantity_rule");
    };

    const int64_t step_raw = rule.increment.raw_value();
    if (step_raw <= 0 || !rule.minimum || !rule.maximum ||
        *rule.minimum > *rule.maximum) {
        return invalid("invalid quantity rule increment or bounds");
    }
    switch (rule.mode) {
        case QuantityRoundingMode::reject_off_increment:
        case QuantityRoundingMode::toward_zero:
        case QuantityRoundingMode::nearest_ties_away_from_zero:
            break;
        case QuantityRoundingMode::unspecified:
        default:
            return invalid("unrecognized quantity rounding mode");
    }
    if (value < *rule.minimum || value > *rule.maximum) {
        return invalid("submitted quantity outside permitted bounds");
    }

    const int64_t input = value.raw_value();
    const bool negative = input < 0;
    const uint64_t magnitude = negative ? uint64_t{0} - static_cast<uint64_t>(input)
                                        : static_cast<uint64_t>(input);
    const uint64_t step = static_cast<uint64_t>(step_raw);
    const uint64_t remainder = magnitude % step;
    if (rule.mode == QuantityRoundingMode::reject_off_increment && remainder != 0) {
        return invalid("quantity is off increment");
    }

    uint64_t rounded = magnitude - remainder;
    // Comparing against step - remainder avoids doubling a potentially huge
    // remainder. Unsigned addition also accommodates the INT64_MIN magnitude.
    if (rule.mode == QuantityRoundingMode::nearest_ties_away_from_zero &&
        remainder != 0 && remainder >= step - remainder) {
        rounded += step;
    }

    constexpr uint64_t positive_limit =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    constexpr uint64_t negative_limit = positive_limit + 1;
    if (rounded > (negative ? negative_limit : positive_limit)) {
        return invalid("normalized quantity overflows int64");
    }
    const int64_t normalized_raw =
        negative ? (rounded == negative_limit
                        ? std::numeric_limits<int64_t>::min()
                        : -static_cast<int64_t>(rounded))
                 : static_cast<int64_t>(rounded);
    const Quantity normalized = Quantity::from_raw(normalized_raw);
    if (normalized < *rule.minimum || normalized > *rule.maximum) {
        return invalid("normalized quantity outside permitted bounds");
    }
    return normalized;
}

}  // namespace trade_ngin
