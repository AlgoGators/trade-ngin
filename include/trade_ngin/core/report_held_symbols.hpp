// include/trade_ngin/core/report_held_symbols.hpp
#pragma once

#include <string>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief The contracts the Symbols Reference lists: the book's positions plus every contract any
 *        sleeve holds. A contract the sleeves hold in opposite directions nets to
 *        zero in the book's map, and the book's own entry for it is a zero row: it is replaced by
 *        the sleeve's, so the contract is listed.
 *
 * @tparam Positions     an unordered_map<string, Position>
 * @tparam SleevesByName an unordered_map<string, Positions>
 */
template <typename Positions, typename SleevesByName>
Positions held_by_any_sleeve(const Positions& book, const SleevesByName& sleeves) {
    Positions held = book;
    for (const auto& sleeve : sleeves) {
        for (const auto& [symbol, position] : sleeve.second) {
            if (position.quantity.as_double() == 0.0) continue;
            auto [at, added] = held.emplace(symbol, position);
            if (!added && at->second.quantity.as_double() == 0.0) at->second = position;
        }
    }
    return held;
}

}  // namespace trade_ngin
