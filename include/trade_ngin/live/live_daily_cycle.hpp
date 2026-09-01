// include/trade_ngin/live/live_daily_cycle.hpp
#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/live/execution_manager.hpp"
#include "trade_ngin/live/execution_price_resolver.hpp"
#include "trade_ngin/live/live_pnl_manager.hpp"
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

    /** What one day's execution step did, beyond the fills themselves. */
    struct ExecutionOutcome {
        std::vector<ExecutionReport> executions;
        /** Symbols priced from an older real session: "SYM @ date (N days stale)". */
        std::vector<std::string> widened_prices;
        /** Symbols with no usable price. These did not trade. */
        std::vector<std::string> unpriced;
        /** Symbols whose day-T target was rolled back because they did not trade. */
        std::vector<std::string> rolled_back;
        /**
         * The prices the fills were actually generated at, T-1 closes plus any
         * widened substitutes. Returned so the caller marks positions with the same
         * numbers it traded at: marking from a different map than the one that priced
         * the executions is how a log-versus-DB disagreement starts.
         */
        std::unordered_map<std::string, double> execution_prices;
    };

    /**
     * @brief Price the day's trades, generate them, and keep the book honest about
     *        what did not happen.
     *
     * Three rules, in order, each of which was learned from a defect:
     *
     * 1. A fill is priced from a REAL close or it is not priced at all. The previous
     *    behaviour fell back to Position::average_price -- a cost basis, and 0 for a
     *    position opened today -- which booked fills at zero, persisted the zero as
     *    the new basis, and reloaded it the next session as a carried basis of zero.
     *
     * 2. A missing T-1 close is not automatically fatal. Halts, thin names and the
     *    session after a holiday leave a symbol without a print but with a perfectly
     *    good older close, and that close is a real price. It is substituted only
     *    within a staleness bound and only when T-1 is genuinely absent.
     *
     * 3. A symbol that could not be priced did not trade, so the day-T book must not
     *    claim it did. Its target is rolled back to the carried quantity (or dropped
     *    if it was never held). Without this the runner persists a position no
     *    execution supports -- a phantom that reads back next session as real.
     *
     * @param positions [in,out] the day-T target book. Rolled back in place for any
     *        symbol that could not be priced.
     */
    static Result<ExecutionOutcome> execute_day_t(
        ExecutionManager& execution_manager,
        std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::unordered_map<std::string, double>& t1_closes,
        const std::vector<Bar>& bars,
        const Timestamp& now,
        int max_staleness_days = ExecutionPriceResolver::kDefaultMaxStalenessDays) {

        ExecutionOutcome outcome;

        std::set<std::string> needed;
        for (const auto& [symbol, position] : positions) {
            if (position.quantity.as_double() != 0.0) needed.insert(symbol);
        }
        for (const auto& [symbol, position] : previous_positions) {
            if (position.quantity.as_double() != 0.0) needed.insert(symbol);
        }

        std::unordered_map<std::string, double> execution_prices = t1_closes;
        auto widened = ExecutionPriceResolver::latest_close_at_or_before(bars, now);
        auto fill = ExecutionPriceResolver::fill_missing(execution_prices, widened, needed, now,
                                                         max_staleness_days);
        outcome.widened_prices = fill.widened;
        outcome.execution_prices = execution_prices;

        auto exec_result = execution_manager.generate_daily_executions(
            positions, previous_positions, execution_prices, now, &outcome.unpriced);
        if (exec_result.is_error()) {
            return make_error<ExecutionOutcome>(exec_result.error()->code(),
                                                exec_result.error()->what(),
                                                "LiveDailyCycle::execute_day_t");
        }
        outcome.executions = exec_result.value();

        // Rule 3: the book must not record a trade that did not happen.
        for (const auto& symbol : outcome.unpriced) {
            auto prev_it = previous_positions.find(symbol);
            if (prev_it != previous_positions.end() &&
                prev_it->second.quantity.as_double() != 0.0) {
                positions[symbol] = prev_it->second;
                positions[symbol].last_update = now;
            } else {
                positions.erase(symbol);
            }
            outcome.rolled_back.push_back(symbol);
        }

        return Result<ExecutionOutcome>(outcome);
    }

    /**
     * @brief The cost basis to persist for each day-T position, and why.
     *
     * Runs AFTER executions have been fed back through on_execution(), which is what
     * gives a symbol that traded today its weighted-average basis. Everything else is
     * a carried holding whose basis came from the seeded book.
     *
     * The residual -- a held position neither source knows -- should be unreachable.
     * It is handled rather than ignored because the previous code ignored it, and
     * "ignore" meant leaving the day-T placeholder in place. That placeholder was the
     * previous close, so the one path that could not find a basis was the one path
     * that silently substituted a mark for it.
     *
     * @param positions [in,out] day-T book; average_price and unrealized_pnl written.
     * @return symbols that hit the residual, for the caller to log loudly.
     */
    static std::vector<std::string> resolve_and_apply_basis(
        std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, Position>& strategy_positions,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::unordered_map<std::string, double>& marks) {

        std::vector<std::string> unresolved;

        for (auto& [symbol, position] : positions) {
            double strategy_basis = 0.0;
            auto strat_it = strategy_positions.find(symbol);
            if (strat_it != strategy_positions.end()) {
                strategy_basis = strat_it->second.average_price.as_double();
            }

            double carried_basis = 0.0;
            auto carried_it = previous_positions.find(symbol);
            if (carried_it != previous_positions.end() &&
                carried_it->second.quantity.as_double() != 0.0) {
                carried_basis = carried_it->second.average_price.as_double();
            }

            double basis = LivePnLManager::resolve_day_t_cost_basis(strategy_basis, carried_basis);

            if (basis > 0.0) {
                position.average_price = Decimal(basis);
                double mark = 0.0;
                auto mark_it = marks.find(symbol);
                if (mark_it != marks.end()) mark = mark_it->second;
                if (mark > 0.0) {
                    position.unrealized_pnl = Decimal(LivePnLManager::unrealized_from_cost_basis(
                        position.quantity.as_double(), basis, mark));
                }
            } else if (position.quantity.as_double() != 0.0) {
                position.average_price = Decimal(0.0);
                position.unrealized_pnl = Decimal(0.0);
                unresolved.push_back(symbol);
            }

            if (strat_it != strategy_positions.end()) {
                position.realized_pnl = strat_it->second.realized_pnl;
            }
        }

        std::sort(unresolved.begin(), unresolved.end());
        return unresolved;
    }
};

}  // namespace trade_ngin
