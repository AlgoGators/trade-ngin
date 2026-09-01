// include/trade_ngin/live/live_daily_cycle.hpp
#pragma once

#include <unordered_map>
#include <string>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

/**
 * @brief Ordering rules for one live trading day.
 *
 * These live in a named place because they are ordering constraints, not
 * computations: getting them wrong produces a runner that reads correctly
 * line by line and is still silently wrong. Extracted from
 * apps/strategies/live_equity_mean_reversion.cpp so they can be tested
 * without standing up a database and a full trading day.
 */
class LiveDailyCycle {
public:
    /**
     * @brief Put a live strategy into the state signal generation assumes,
     *        then generate the day's signals.
     *
     * A live runner is a fresh process every session: BaseStrategy::positions_
     * starts empty and on_execution() -- its only writer -- does not run until
     * AFTER targets have been extracted. A strategy whose signal depends on what
     * it currently holds therefore sees an empty book on every bar of every
     * session unless the previous day's holdings are seeded back in first.
     *
     * MeanReversionStrategy::generate_signal() is exactly such a strategy: it
     * branches on positions_ to choose between its entry rule and its exit rule.
     * Unseeded it takes the entry branch forever, so a held position is
     * liquidated as soon as its z-score leaves the entry zone -- at the ENTRY
     * threshold rather than the (much closer to zero) EXIT threshold -- and the
     * stop-loss, which measures from Position::average_price, can never fire
     * because there is no position to measure. Backtests are unaffected: one
     * process runs the whole history, so positions_ accumulates through
     * on_execution() and is never empty at the wrong moment.
     *
     * @param previous_positions The previous day's book AFTER corporate actions
     *        have been applied. Order matters here too: splits restate quantity
     *        and dividends restate cost basis, so seeding the pre-adjustment
     *        snapshot would anchor the strategy to a book that no longer exists.
     * @param bars The day's market data.
     */
    static Result<void> prepare_strategy_for_signals(
        BaseStrategy& strategy,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::vector<Bar>& bars) {
        auto seeded = strategy.seed_positions(previous_positions);
        if (seeded.is_error()) return seeded;
        return strategy.on_data(bars);
    }
};

}  // namespace trade_ngin
