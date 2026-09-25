// include/trade_ngin/backtest/equity_cost_retier.hpp
//
// K1 (T-4b BT-cost-tier-warmup; STAGE3_PLAN §25a item 3, T-7b-2 8c): the equity backtest re-tiers
// every symbol's liquidity-tiered cost config on every bar, from the 20 bars ending at the bar the
// fill is priced at, in split-consistent share units, on BOTH cost managers that price a backtest
// fill (the coordinator's execution manager's and the PortfolioManager's own, E2-C9).
//
// Before this, bt_equity_mr registered each symbol's tier ONCE, before the run, from the 20 bars
// before start_date (equity_cost_warmup.hpp), and that tier priced every fill of a two-year window
// while live re-tiers on every run from the 20 bars ending at T-1
// (register_equity_costs_from_bars). A symbol whose liquidity moved kept its start-of-window tier
// (ABT: LARGE in the warm-up, MEGA later).
//
// The unit. The backtest's prices and quantities are back-adjusted to the window's end
// (market_data_utils.cpp build_equity_adjusted_query: a bar's price is divided by the split factor
// of every LATER bar), but the loader leaves volume raw, so a pre-split bar's raw share volume is
// in a different share unit from its price (BKNG's raw volume jumps 25x at its 2026-04-06 split).
// Here a bar's volume is expressed in the window-end share unit: raw volume x the product of the
// split factors of every ex-date STRICTLY AFTER the bar's own date, up to the run's end date (a
// bar ON its ex-date already trades in the new shares). That is the unit of the adjusted price and
// of the traded quantity. The same split-consistent volume feeds the impact model's ADV in the
// equity path (T-4b ADVERSARIAL A-3: the ADV that picks the tier and the ADV that scales
// participation are then one unit, as live's are the same twenty observations).
//
// Frames, stated (T-4b ADVERSARIAL A-2): the backtest tiers in the END-DATE frame, live in the
// T-1 frame; they coincide on every bar with no later split inside the window. Live's own
// 20-session mixed-unit window after a split is not changed here.
//
// No hysteresis band (T-4b: it bought $0.0012 and would put the backtest out of step with live's
// stateless re-tier). The tier's registration price is the window's last close (it only decides
// the sub-$1 tick rule).

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"

namespace trade_ngin {
namespace backtest {

/// register_equity_costs_from_bars' default lookback: the tier's ADV is the mean of the last 20.
inline constexpr size_t kEquityCostRetierBars = 20;

/// The tier get_tiered_equity_config picks for an ADV (asset_cost_config.cpp thresholds).
inline std::string equity_cost_tier_of_adv(double adv) {
    if (adv <= 0.0) return "DEFAULT";
    if (adv > 10000000.0) return "MEGA";
    if (adv > 2000000.0) return "LARGE";
    if (adv > 500000.0) return "MID";
    if (adv > 100000.0) return "SMALL";
    return "PENNY";
}

/// The tier a registered config came from, read back from the fields get_tiered_equity_config
/// sets; "OTHER" for any config it did not produce (a futures config, the untiered default).
inline std::string equity_cost_tier_of_config(const transaction_cost::AssetCostConfig& c) {
    for (const auto& [adv, name] : std::vector<std::pair<double, const char*>>{
             {2.0e7, "MEGA"}, {5.0e6, "LARGE"}, {1.0e6, "MID"}, {3.0e5, "SMALL"}, {5.0e4, "PENNY"}}) {
        const auto t = transaction_cost::AssetCostConfigRegistry::get_tiered_equity_config(100.0, adv);
        if (c.baseline_spread_ticks == t.baseline_spread_ticks &&
            c.min_spread_ticks == t.min_spread_ticks && c.max_spread_ticks == t.max_spread_ticks &&
            c.max_impact_bps == t.max_impact_bps &&
            c.max_total_implicit_bps == t.max_total_implicit_bps) {
            return name;
        }
    }
    return "OTHER";
}

class EquityCostRetier {
public:
    struct Change {
        std::string symbol;
        std::string from;  ///< the tier the managers held before this registration
        std::string to;
        double adv = 0.0;  ///< mean split-consistent volume of the window
        size_t bars = 0;
        std::string first_bar;  ///< YYYY-MM-DD (UTC) of the window's first and last bar
        std::string last_bar;
    };

    struct Pass {
        size_t registered = 0;  ///< symbols registered on each manager
        std::vector<Change> changes;
    };

    void reset() {
        splits_.clear();
        trailing_.clear();
        registered_tier_.clear();
    }

    /// One split event: raw share volume on bars before `ex_date` is multiplied by `factor` to
    /// reach the share unit of bars on or after it. A factor of 0, 1 or below 0 is no event.
    void add_split(const std::string& symbol, const std::string& ex_date, double factor) {
        if (!(factor > 0.0) || factor == 1.0) return;
        splits_[symbol].emplace_back(ex_date, factor);
    }

    size_t split_count() const {
        size_t n = 0;
        for (const auto& [_, v] : splits_) n += v.size();
        return n;
    }

    /// The product of the split factors of every ex-date strictly after `bar_date` (YYYY-MM-DD).
    double volume_multiplier(const std::string& symbol, const std::string& bar_date) const {
        auto it = splits_.find(symbol);
        if (it == splits_.end()) return 1.0;
        double m = 1.0;
        for (const auto& [ex_date, factor] : it->second) {
            if (ex_date > bar_date) m *= factor;
        }
        return m;
    }

    /// The bar's volume in the window-end share unit.
    double split_consistent_volume(const Bar& bar) const {
        return bar.volume * volume_multiplier(bar.symbol, core::format_utc_date(bar.timestamp));
    }

    /// Appends each bar (volume in the window-end share unit) to its symbol's trailing window,
    /// keeping the last 20. Call with bars in date order.
    void append(const std::vector<Bar>& bars) {
        for (const auto& bar : bars) {
            Bar b = bar;
            b.volume = split_consistent_volume(bar);
            auto& window = trailing_[b.symbol];
            window.push_back(std::move(b));
            while (window.size() > kEquityCostRetierBars) window.pop_front();
        }
    }

    /// Registers, on both managers, every symbol with at least one bar in its trailing window,
    /// through the same register_equity_costs_from_bars live uses (the mean of the last
    /// min(20, n) split-consistent volumes picks the tier). A symbol with no bar yet keeps
    /// whatever the managers hold.
    Pass retier(transaction_cost::TransactionCostManager& execution_costs,
                transaction_cost::TransactionCostManager& portfolio_costs) {
        Pass pass;
        std::vector<std::string> symbols;
        std::unordered_map<std::string, std::vector<Bar>> bars_by_symbol;
        for (const auto& [symbol, window] : trailing_) {
            if (window.empty()) continue;
            symbols.push_back(symbol);
            bars_by_symbol[symbol] = std::vector<Bar>(window.begin(), window.end());
        }
        if (symbols.empty()) return pass;

        for (const auto& symbol : symbols) {
            const auto& window = bars_by_symbol.at(symbol);
            double sum = 0.0;
            for (const auto& b : window) sum += b.volume;
            const double adv = sum / static_cast<double>(window.size());
            const std::string to = equity_cost_tier_of_adv(adv);
            auto held = registered_tier_.find(symbol);
            const std::string from =
                held != registered_tier_.end()
                    ? held->second
                    : equity_cost_tier_of_config(execution_costs.get_asset_config(symbol));
            if (from != to) {
                Change c;
                c.symbol = symbol;
                c.from = from;
                c.to = to;
                c.adv = adv;
                c.bars = window.size();
                c.first_bar = core::format_utc_date(window.front().timestamp);
                c.last_bar = core::format_utc_date(window.back().timestamp);
                pass.changes.push_back(std::move(c));
            }
            registered_tier_[symbol] = to;
        }

        const int lookback = static_cast<int>(kEquityCostRetierBars);
        execution_costs.register_equity_costs_from_bars(symbols, bars_by_symbol, lookback);
        portfolio_costs.register_equity_costs_from_bars(symbols, bars_by_symbol, lookback);
        pass.registered = symbols.size();
        return pass;
    }

private:
    std::map<std::string, std::vector<std::pair<std::string, double>>> splits_;
    std::map<std::string, std::deque<Bar>> trailing_;
    std::map<std::string, std::string> registered_tier_;
};

}  // namespace backtest
}  // namespace trade_ngin
