// include/trade_ngin/backtest/junk_signal_feed.hpp
#pragma once

#include <set>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief What the futures backtest feeds its strategies, its PortfolioManager and its cost models
 *        on one cycle under LOOP_SPEC v6.1 section 2.1 (K-01, LOCKED).
 *
 * A JUNK bar and a thin first print (SymbolDayVerdict::k01_withheld) are WITHHELD: never consumed
 * and never fed later. The live runners take the same bars out of their whole window on every run
 * (session_classifier.hpp, k01_consumed_bars), so neither engine ever forms a return across a
 * withheld bar: the next consumed bar's return is against the last consumed close.
 *
 *   - feed:     the signal group without the withheld symbols' bars, the other bars in their order;
 *   - withheld: those bars, in the group's order (logged, never carried to a later cycle).
 *
 * This replaces T-7b-1 commit 7a's one-cycle delayed feed, which fed a withheld bar on the next
 * cycle ahead of its symbol's next bar (K-01 retires that release).
 */
struct K01SignalFeed {
    std::vector<Bar> feed;
    std::vector<Bar> withheld;
};

inline K01SignalFeed k01_signal_feed(const std::vector<Bar>& signal_group,
                                     const std::set<std::string>& withheld_symbols) {
    K01SignalFeed out;
    out.feed.reserve(signal_group.size());
    for (const auto& b : signal_group) {
        if (withheld_symbols.count(b.symbol)) {
            out.withheld.push_back(b);
        } else {
            out.feed.push_back(b);
        }
    }
    return out;
}

}  // namespace backtest
}  // namespace trade_ngin
