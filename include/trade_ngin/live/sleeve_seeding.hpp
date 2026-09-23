// include/trade_ngin/live/sleeve_seeding.hpp
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"

namespace trade_ngin {

/// Loads one sleeve's stored book of the previous day (the futures runners pass
/// db->load_positions_by_date(combined_strategy_id, <sleeve>, portfolio_id, now - 24h,
/// "trading.positions")).
using SleeveBookLoader = std::function<Result<std::unordered_map<std::string, Position>>(
    const std::string& strategy_name)>;

/// What seed_every_sleeve did for one sleeve.
struct SleeveSeed {
    std::string strategy_name;
    size_t rows{0};    ///< stored rows loaded for the sleeve (0: nothing was seeded)
    int pm_seeded{0};  ///< of those, how many the PortfolioManager accepted into the sleeve's slot
};

/**
 * @brief Seeds EVERY sleeve of a live book from that sleeve's own stored rows of the previous day.
 *
 * Each live invocation is a fresh process, so a strategy's positions_ and the PortfolioManager's
 * per-sleeve current_positions start at zero. The held-book seed (eae0d5a7) puts the stored book
 * back in both places before the day's rebalance:
 *   Fix #1  strategies[i]->seed_positions(rows): the Carver position buffer reads positions_ as
 *           its comparison anchor; without it the buffer cannot absorb day-to-day signal jitter.
 *   Fix #7  portfolio.update_strategy_position(strategy_names[i], symbol, row) per row: the
 *           optimizer's current_weights are summed from each sleeve's current_positions; without
 *           it the optimizer's coordinate descent starts from current=0 every fresh process and
 *           converges to a zero-anchored compromise (e.g. MYM=2 instead of yesterday's actual=1),
 *           producing daily +/-1 trades as market noise crosses rounding boundaries.
 * Neither mechanism is changed here. What changed (T-7a C3, T-BASE finding): the runners seeded
 * strategy_names[0] only, so on a multi-sleeve book (BASE: TREND_FOLLOWING and
 * TREND_FOLLOWING_FAST) the second sleeve was never seeded, in either place, and the optimizer
 * compared a `current` summed over one sleeve against a `target` summed over two.
 *
 * strategies[i] was created from strategy_names[i] (the runners' factory loop) and the PM's sleeve
 * key is that same name (the strategy's metadata.id), so the strategy object, the stored rows'
 * strategy_name and the PM slot agree. The sleeves are visited in strategy_names order; each sleeve
 * logs exactly the lines the one-sleeve seed logged, so a one-sleeve book (CONSERVATIVE) is unchanged.
 *
 * NOTE (ledger BASE-opposite-sleeve-anchor; designed with netting in T-7b, not here): two sleeves
 * may hold OPPOSITE signs on one symbol (stored on BASE: ZT.v.0 2025-11-08 and 11-09, TREND_FOLLOWING
 * +1 / TREND_FOLLOWING_FAST -1; MBT.v.0 2025-01-29, +4 / -8). This seed does not net them: each
 * sleeve gets its own signed row in its own slot, so the optimizer's anchor (current_weights, the
 * sum over sleeves) is the NET position, while the runner's per-sleeve execution diff charges each
 * sleeve's leg gross. HD's ruled shape (2026-09-19: sleeves kept gross per sleeve for attribution,
 * the account holds the net, the risk gate sizes on the net, the cross logged at zero cost) is not
 * implemented by this function and must not be assumed from it.
 *
 * @return one record per sleeve, in strategy_names order.
 */
inline std::vector<SleeveSeed> seed_every_sleeve(
    const std::vector<std::shared_ptr<StrategyInterface>>& strategies,
    const std::vector<std::string>& strategy_names, PortfolioManager& portfolio,
    const SleeveBookLoader& load_book) {
    std::vector<SleeveSeed> seeds;
    const size_t n = std::min(strategies.size(), strategy_names.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string& seed_strategy_name = strategy_names[i];
        SleeveSeed seed;
        seed.strategy_name = seed_strategy_name;
        auto seed_result = load_book(seed_strategy_name);
        if (seed_result.is_ok() && !seed_result.value().empty()) {
            seed.rows = seed_result.value().size();
            // Fix #1: seed the sleeve's positions_ for buffer correctness
            auto seeded = strategies[i]->seed_positions(seed_result.value());
            if (seeded.is_error()) {
                WARN("Failed to seed strategy positions for " + seed_strategy_name + ": " +
                     std::string(seeded.error()->what()));
            }
            // Fix #7: seed the sleeve's slot of PortfolioManager.current_positions for
            // optimizer-baseline correctness. PortfolioManager.strategies_ is keyed by the
            // strategy's metadata.id (e.g. "TREND_FOLLOWING"), NOT the combined_strategy_id
            // used for the DB rows (e.g. "LIVE_TREND_FOLLOWING").
            for (const auto& [sym, pos] : seed_result.value()) {
                auto pm_seed = portfolio.update_strategy_position(seed_strategy_name, sym, pos);
                if (pm_seed.is_error()) {
                    WARN("Failed to seed PortfolioManager position for " + sym + ": " +
                         std::string(pm_seed.error()->what()));
                } else {
                    seed.pm_seeded++;
                }
            }
            INFO("Seeded " + std::to_string(seed.pm_seeded) +
                 " positions into PortfolioManager.current_positions for "
                 "optimizer-baseline correctness (strategy_name=" +
                 seed_strategy_name + ")");
        } else {
            INFO("No yesterday positions to seed for strategy " + seed_strategy_name +
                 " (first run or no data)");
        }
        seeds.push_back(seed);
    }
    return seeds;
}

}  // namespace trade_ngin
