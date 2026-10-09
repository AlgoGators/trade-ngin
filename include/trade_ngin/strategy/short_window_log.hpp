#pragma once

// LOOP_SPEC section 1 ("History and seeds"): a symbol with fewer than W = 3,200 consumed bars
// starts at its first bar, and its rows are flagged and counted. The count used to exist only in
// the acceptance record a gate asks for; this is the one INFO line every futures run prints.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/trend_estimator.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace trade_ngin {

/// "ESTIMATOR_SHORT_WINDOW window=3200 symbols=<n> rows=<r> names=[<symbol>:<rows> ...]": the
/// symbols a trend sleeve sized on a window shorter than W in this run, in name order, each with
/// its rows (the largest count among the sleeves), and their total. "names=[-]" when there is none.
inline std::string estimator_short_window_line(
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies) {
    std::map<std::string, std::size_t> rows;
    for (const auto& strategy : strategies) {
        const auto trend = std::dynamic_pointer_cast<TrendFollowingStrategy>(strategy);
        if (!trend) continue;
        for (const auto& [symbol, n] : trend->short_window_rows()) {
            if (n > rows[symbol]) rows[symbol] = n;
        }
    }
    std::size_t total = 0;
    std::string names;
    for (const auto& [symbol, n] : rows) {
        if (n == 0) continue;
        total += n;
        names += (names.empty() ? "" : " ") + symbol + ":" + std::to_string(n);
    }
    std::size_t count = 0;
    for (const auto& [symbol, n] : rows) count += n > 0 ? 1 : 0;
    return "ESTIMATOR_SHORT_WINDOW window=" + std::to_string(trend_estimator::kWindowBars) +
           " symbols=" + std::to_string(count) + " rows=" + std::to_string(total) + " names=[" +
           (names.empty() ? std::string("-") : names) + "]";
}

}  // namespace trade_ngin
