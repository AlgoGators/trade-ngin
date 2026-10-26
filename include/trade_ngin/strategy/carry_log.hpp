#pragma once

// Carry (trade-ngin #185, HD's Q1): the equity slow rule acts on the trend part of the blend only, so an
// equity index the rule names can go short on carry while its trend part alone is not short. Such shorts
// are allowed, counted and reported: this is the one INFO line a futures run prints for them, and only
// when a sleeve has carry on (a carry-off run prints nothing new).

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace trade_ngin {

/// "CARRY_DRIVEN_EQUITY_SHORTS symbols=<n> rows=<r> names=[<symbol>:<rows> ...]": per symbol, the signal
/// bars of this run on which a carry-on sleeve's blended forecast of a ruled equity was short while its
/// trend part was not (the largest count among the sleeves), and their total; "names=[-]" when there is
/// none. Empty when no sleeve has carry on.
inline std::string carry_driven_shorts_line(
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {
    bool carry = false;
    std::map<std::string, std::size_t> rows;
    for (const auto& strategy : strategies) {
        const auto trend = std::dynamic_pointer_cast<TrendFollowingStrategy>(strategy);
        if (!trend || !trend->carry_on()) continue;
        carry = true;
        for (const auto& [symbol, n] : trend->carry_driven_shorts()) {
            if (n > rows[symbol]) rows[symbol] = n;
        }
    }
    if (!carry) return {};
    std::size_t total = 0;
    std::string names;
    for (const auto& [symbol, n] : rows) {
        total += n;
        names += (names.empty() ? "" : " ") + symbol + ":" + std::to_string(n);
    }
    return "CARRY_DRIVEN_EQUITY_SHORTS symbols=" + std::to_string(rows.size()) +
           " rows=" + std::to_string(total) + " names=[" + (names.empty() ? std::string("-") : names) + "]";
}

}  // namespace trade_ngin
