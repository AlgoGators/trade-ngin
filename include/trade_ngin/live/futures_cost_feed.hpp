// include/trade_ngin/live/futures_cost_feed.hpp
#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {

/**
 * @brief K2 redesigned (T-7b-1 C8a, HD 2026-09-17 / 2026-09-19): the live futures runners'
 *        cost feed, with two inputs.
 *
 * The cost of a fill of q contracts at the T-1 close P (transaction_cost_manager.cpp,
 * impact_model.cpp, spread_model.cpp):
 *
 *   impact  = min(k_bps(V) * sqrt(clamp(q / max(V, 100), 0, 0.1)), max_impact_bps) / 1e4 * P
 *   spread  = spread_cost_multiplier * clamp(baseline_ticks * vol_mult, min_ticks, max_ticks)
 *             * tick_size
 *   implicit = spread + impact (capped at max_total_implicit_bps); slippage = implicit * q *
 *   point_value; total = commission + slippage. k_bps(V) is the liquidity tier (10/20/40/60/80
 *   bps for V above 1M/200k/50k/20k/below).
 *
 * The two inputs, as ruled:
 *
 *   V        the FILL DAY'S OWN VOLUME: the volume of the symbol's latest bar in the feed,
 *            which is the T-1 bar whose close prices the fill. It prices the participation
 *            term AND keys the k_bps tier, so a thin session is charged its own cost. The
 *            impact model's window is given exactly this one observation, so its "ADV" is V.
 *   vol_mult from the walk of the symbol's consecutive log returns ending at that T-1 bar:
 *            the spread model keeps the last 20 (SpreadModel lookback_days), so vol_mult =
 *            clip(1 + 0.15 * clip((stdev_20 - 0.01) / 0.005, -2, 2), 0.8, 1.5). The first bar
 *            of a symbol has no previous close, so it contributes no return (nothing
 *            fabricated); fewer than 2 returns leave the model's neutral 1.0.
 *
 * Before this, both runners fed ONE bar per symbol through ExecutionManager's 3-arg form: V was
 * the same own-day volume, and the one return was log(close / close) = 0, so vol_mult was
 * always 1.0. The impact term is therefore unchanged by construction; only the spread term
 * moves, through vol_mult.
 *
 * The feed is the runner's strategy feed (a JUNK symbol's T-1 bar withheld, T-7b-1 C7b R10),
 * already one bar per symbol-instant (the futures loader keeps the max-volume copy, T-6c B0). It
 * is NOT de-duplicated again here: a repeated instant is fed as given and reported, so a loader
 * regression shows in the log instead of being papered over.
 *
 * Only the cost manager passed in is fed (the live runners pass the ExecutionManager's). The
 * PortfolioManager's cost manager, the backtest's two managers and the equity feed
 * (LiveDailyCycle::feed_cost_model) are untouched.
 */
struct FuturesCostFeedSymbol {
    std::string symbol;
    size_t bars = 0;             ///< bars of this symbol in the feed
    size_t returns = 0;          ///< log returns handed to the volatility window (all of them;
                                 ///< the window keeps the last 20)
    double own_day_volume = 0.0; ///< the latest bar's volume: the impact model's only input
    Timestamp own_day_time{};    ///< the latest bar's timestamp
};

struct FuturesCostFeedResult {
    std::vector<FuturesCostFeedSymbol> symbols;  ///< one entry per fed symbol, by symbol
    size_t returns_fed = 0;
    std::vector<std::string> thin;               ///< fewer than min_bars bars (< 20 returns)
    std::vector<std::string> repeated_instants;  ///< a symbol with two bars at one instant
};

inline FuturesCostFeedResult feed_futures_cost_model(
    transaction_cost::TransactionCostManager& tcm, const std::vector<Bar>& feed_bars,
    size_t min_bars = 21) {
    std::map<std::string, std::vector<Bar>> by_symbol;
    for (const auto& bar : feed_bars) by_symbol[bar.symbol].push_back(bar);

    FuturesCostFeedResult out;
    for (auto& [symbol, bars] : by_symbol) {
        std::stable_sort(bars.begin(), bars.end(),
                         [](const Bar& a, const Bar& b) { return a.timestamp < b.timestamp; });

        FuturesCostFeedSymbol entry;
        entry.symbol = symbol;
        entry.bars = bars.size();

        // The volatility term: every consecutive return, oldest first; the window keeps the
        // last 20, which end at the T-1 bar. 0.0 = "no previous close" on the first bar.
        double prev_close = 0.0;
        for (size_t i = 0; i < bars.size(); ++i) {
            if (i > 0 && bars[i].timestamp == bars[i - 1].timestamp &&
                (out.repeated_instants.empty() || out.repeated_instants.back() != symbol)) {
                out.repeated_instants.push_back(symbol);
            }
            const double close = static_cast<double>(bars[i].close);
            tcm.record_log_return(symbol, close, prev_close);
            if (prev_close > 0.0 && close > 0.0) ++entry.returns;
            prev_close = close;
        }

        // The impact term: the fill day's own volume, one observation.
        entry.own_day_volume = bars.back().volume;
        entry.own_day_time = bars.back().timestamp;
        tcm.record_volume(symbol, entry.own_day_volume);

        out.returns_fed += entry.returns;
        if (bars.size() < min_bars) out.thin.push_back(symbol);
        out.symbols.push_back(entry);
    }
    return out;
}

}  // namespace trade_ngin
