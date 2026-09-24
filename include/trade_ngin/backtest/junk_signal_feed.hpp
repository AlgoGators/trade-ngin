// include/trade_ngin/backtest/junk_signal_feed.hpp
#pragma once

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"

namespace trade_ngin {
namespace backtest {

/**
 * @brief What the futures backtest feeds its strategies and PortfolioManager on one cycle, under
 *        the live runners' JUNK rule (T-7b-1 commit 7a; HD 2026-09-24; T-7a_CODE_REVIEW R1).
 *
 * Live: every run is a fresh process that feeds its whole history window; a JUNK symbol's T-1 bar
 * is taken out of that feed (withhold_junk_t1_bars, session_book_gate.hpp), so its signal is not
 * updated that day, and the NEXT run feeds the same bar again as T-2, with the rest of the
 * history, in date order. The backtest feeds one signal group per cycle to stateful strategies,
 * so the same rule is a one-cycle delayed feed:
 *
 *   - feed:     the signal group without the JUNK symbols' bars, plus every bar withheld on the
 *               previous cycle, each placed right before its symbol's first bar in the group (so it
 *               is fed in date order and the order of the other symbols' bars is unchanged), or at
 *               the end when its symbol has no fed bar in the group;
 *   - withheld: this group's JUNK symbols' bars, to be passed back as `carried` next cycle (a
 *               symbol JUNK again is withheld again while its older bar is fed);
 *   - released: the carried bars that entered this feed (all of them).
 *
 * A carried bar comes from an earlier group, so it is older than any bar of the group; one that is
 * not older (never produced by the coordinator) still goes in by date, after the group's bar.
 */
struct JunkSignalFeed {
    std::vector<Bar> feed;
    std::vector<Bar> withheld;
    std::vector<Bar> released;
};

inline JunkSignalFeed junk_delayed_signal_feed(const std::vector<Bar>& signal_group,
                                               const std::set<std::string>& junk_symbols,
                                               const std::vector<Bar>& carried) {
    JunkSignalFeed out;
    std::map<std::string, std::vector<Bar>> pending;
    for (const auto& b : carried) pending[b.symbol].push_back(b);
    for (auto& [symbol, bars] : pending) {
        (void)symbol;
        std::stable_sort(bars.begin(), bars.end(),
                         [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });
    }
    out.released = carried;
    out.feed.reserve(signal_group.size() + carried.size());
    for (const auto& b : signal_group) {
        if (junk_symbols.count(b.symbol)) {
            out.withheld.push_back(b);
            continue;
        }
        auto it = pending.find(b.symbol);
        if (it == pending.end()) {
            out.feed.push_back(b);
            continue;
        }
        std::vector<Bar> later;
        for (const auto& p : it->second) {
            if (p.timestamp < b.timestamp) {
                out.feed.push_back(p);
            } else {
                later.push_back(p);
            }
        }
        out.feed.push_back(b);
        out.feed.insert(out.feed.end(), later.begin(), later.end());
        pending.erase(it);
    }
    for (const auto& [symbol, bars] : pending) {
        (void)symbol;
        out.feed.insert(out.feed.end(), bars.begin(), bars.end());
    }
    return out;
}

}  // namespace backtest
}  // namespace trade_ngin
