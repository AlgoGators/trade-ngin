// include/trade_ngin/live/live_daily_cycle.hpp
#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/live/corporate_actions_lifecycle.hpp"
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
     * @brief Put a live strategy into the state signal generation assumes.
     *
     * Seeding ONLY. This used to call on_data(bars) as well, and the caller then
     * ran PortfolioManager::process_market_data over the same vector, which calls
     * on_data() again (portfolio_manager.cpp). MeanReversionStrategy::on_data
     * appends unconditionally -- price_history.push_back and volume_sample_count++
     * per bar -- so every live session fed each bar twice: the history ran to its
     * trim cap rather than the bar count and the ADV EMA advanced over a series
     * twice as long as the one that traded, which is what the fractional-share
     * eligibility gate reads. A backtest feeds each bar once, so live and backtest
     * could not agree on the same data by construction (E2-F28 / E3 NEW-6).
     *
     * The feed belongs to the portfolio manager, which is the component that then
     * reads the targets. Fixing it here rather than in portfolio_manager.cpp is
     * deliberate: that file is on the futures path, and removing a feed there would
     * change what the trend strategies see.
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
     */
    static Result<void> prepare_strategy_for_signals(
        BaseStrategy& strategy,
        const std::unordered_map<std::string, Position>& previous_positions) {
        // E2-F19 / E2-F20: seed quantity, basis and mark -- never yesterday's realized.
        //
        // BaseStrategy::seed_positions is a wholesale copy and on_execution() adds to
        // whatever it finds, so a seeded realized_pnl made every persisted day-T row
        // "yesterday's row + today's fills": a running total under a column named
        // daily. Worse, on a Monday yesterday's row was Friday's, already rewritten by
        // the Saturday and Sunday finalizations with Friday's MARK MOVE, so a price
        // move entered a column that is supposed to hold trades (measured: META
        // 2026-08-04 = 73.35 Friday mark + 31.31 Mon trade + 15.93 Tue trade).
        //
        // Zeroing it here makes positions_[sym].realized_pnl mean "realized by this
        // symbol in this process", i.e. today. metrics_.realized_pnl -- the aggregate
        // live_results is built from -- was never seeded and does not change.
        //
        // unrealized_pnl stays seeded on purpose: update_metrics() sums it into the
        // drawdown gate, so zeroing it would move a risk limit, not a report.
        //
        // Done in the caller rather than in seed_positions(), which the futures
        // runners call directly: this header is not in their translation units.
        std::unordered_map<std::string, Position> seed_book = previous_positions;
        for (auto& [symbol, position] : seed_book) {
            position.realized_pnl = Decimal(0.0);
        }
        return strategy.seed_positions(seed_book);
    }

    /**
     * @brief The symbols this run must load data for: config plus the successors
     *        of anything currently held.
     *
     * The universe used to be fixed from config before the previous day's book was
     * even read, while `apply_renames` ran ~1,500 lines later. A held position whose
     * successor is not in config therefore got no bars, no instrument, no cost config
     * and no target: the day-T pass reported "Missing T-1 price for symbol with a
     * non-zero position", `execute_day_t` rule 3 rolled the target back to the carried
     * quantity, and the position was carried again the next session and the one after
     * -- an unpriceable zombie, persisted under the new key, that no amount of
     * re-running clears (E2-F34 / E3 F-4). `add_rowless_exits` does not rescue it: its
     * own contract says a symbol that left the universe is "still closed out", and that
     * is only true when a price exists.
     *
     * So the book has to be known BEFORE the universe is finalized. This is the pure
     * part of that ordering: given what is held and the alias table, say which extra
     * tickers the run has to be able to price.
     *
     * The era test is the SAME one apply_renames applies -- same rename map, same
     * as-of guard, same fail-narrow rule -- via CorporateActionsLifecycle::rename_chain,
     * so the universe cannot admit a rename the re-keying will refuse, or miss one it
     * will perform.
     *
     * @param holding_start symbol -> YYYY-MM-DD the CURRENT holding began. Never the
     *        lifetime min(date): a ticker closed in 2021 and re-bought in 2026 would
     *        satisfy the era test for the 2021 alias and the universe would grow a dead
     *        symbol (BA-2 / C-3 D1). A symbol absent here contributes nothing.
     * @return `config_symbols` in their configured order, followed by the successors
     *         that were not already configured, sorted. Deterministic across runs.
     */
    static std::vector<std::string> effective_universe(
        const std::vector<std::string>& config_symbols,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::vector<TickerAlias>& aliases,
        const std::string& as_of_date,
        const std::unordered_map<std::string, std::string>& holding_start) {

        std::vector<std::string> universe = config_symbols;
        std::set<std::string> known(config_symbols.begin(), config_symbols.end());

        const auto renames = CorporateActionsLifecycle::build_rename_map(aliases);
        if (renames.empty()) return universe;

        std::set<std::string> additions;
        for (const auto& [symbol, position] : previous_positions) {
            if (position.quantity.as_double() == 0.0) continue;
            auto start_it = holding_start.find(symbol);
            if (start_it == holding_start.end() || start_it->second.empty()) continue;
            for (const auto& successor : CorporateActionsLifecycle::rename_chain(
                     renames, symbol, start_it->second, as_of_date)) {
                if (known.insert(successor).second) additions.insert(successor);
            }
        }
        universe.insert(universe.end(), additions.begin(), additions.end());
        return universe;
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

        // STRICT: mean reversion's average_price is a weighted cost basis, so the
        // mark fallback would price a new position's fill at 0.00. Futures callers
        // keep MARK_FALLBACK, where that field holds a mark and the fallback is right.
        auto exec_result = execution_manager.generate_daily_executions(
            positions, previous_positions, execution_prices, now,
            PricingPolicy::STRICT, &outcome.unpriced);
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

    /**
     * @brief The prices the day-T rows must be marked at.
     *
     * One map, so a position row and the live_results aggregate cannot disagree about
     * what a symbol was worth. `resolve_and_apply_basis` marks from the execution price
     * map -- T-1 closes plus any widened substitute -- and the aggregate sums those
     * marks; the row loop used to recompute from the T-1 close map alone, which has no
     * entry for a widened symbol at all. That symbol's `daily_unrealized_pnl` was
     * therefore written as 0 while the aggregate carried its real mark, and the in-run
     * L5 assertion covers realized only, so nothing saw it (E2-F35 / BA-4). It fires on
     * exactly the halted and thin names the widening exists to rescue.
     *
     * An EMPTY `execution_prices` -- the closed-day path, where execute_day_t never runs
     * -- returns `t1_closes` unchanged, so the carry-forward day is untouched.
     * Non-positive substitutes are ignored: absent is a better answer than zero, because
     * a zero mark books the whole notional as a gain.
     */
    static std::unordered_map<std::string, double> day_t_mark_prices(
        const std::unordered_map<std::string, double>& t1_closes,
        const std::unordered_map<std::string, double>& execution_prices) {
        std::unordered_map<std::string, double> marks = t1_closes;
        for (const auto& [symbol, price] : execution_prices) {
            if (price > 0.0) marks[symbol] = price;
        }
        return marks;
    }

    // -----------------------------------------------------------------------
    // trading.positions.daily_realized_pnl -- the row-level realized column.
    //
    // DEFINITION. The P&L this position realized on this row's date, gross of
    // transaction costs, under the runner's own accounting model. It is a FLOW:
    // summing it over dates gives the position's realized P&L over that span.
    // daily_unrealized_pnl beside it is a LEVEL and must never be summed over
    // dates.
    //
    //   * Settled book (futures, UnrealizedPolicy::SETTLED): the day's settlement
    //     move qty x (close(D) - close(D-1)) x point_value, written by the T-1
    //     finalization. The futures runners do not use the helpers below.
    //   * Cash book (equities, MARK_TO_MARKET): that day's TRADE-realized from
    //     that date's fills, plus realized locked in by a corporate action on
    //     that date. Written on the day itself and never revised.
    //
    // The helpers below are the equity runner's contract with that definition.
    // Each is a pure function so the rule can be pinned by a unit test rather
    // than only by a database replay (E2-F19 / E2-F20).
    // -----------------------------------------------------------------------

    /** Tolerance below which a quantity or a realized figure counts as zero. */
    static constexpr double kRowTolerance = 1e-10;

    /**
     * @brief Whether a position row carries nothing worth persisting.
     *
     * A row is dead only when it has neither quantity nor realized P&L. The old rule
     * dropped on quantity alone, which is right for futures -- an exit there realizes
     * exactly zero, because average_price is reset to close(T-1) daily and the fill
     * strikes at close(T-1) -- and wrong for equities, where average_price is a true
     * cost basis and the exit realizes the whole accumulated gain. Measured
     * 2026-04-15: TMUS sold out for -402.65, live_results carried it, the positions
     * table did not (E2-F19 route 3).
     *
     * A closed symbol carries realized on the close day only and never has a row
     * again, so this keeps one row per close event, not one per flat day.
     */
    static bool is_dead_row(const Position& position, double tol = kRowTolerance) {
        return std::abs(position.quantity.as_double()) <= tol &&
               std::abs(position.realized_pnl.as_double()) <= tol;
    }

    /**
     * @brief Partition a loaded book into open rows and closed rows.
     *
     * Everything downstream of the load (corporate actions, seeding, execution,
     * basis resolution, the run-gap guard) is written against a book of held
     * positions. Closed rows exist only to carry a realized figure on the date
     * the position closed; they are re-appended to that date's T-1 write set
     * and go nowhere else.
     */
    static void split_open_and_closed(
        const std::unordered_map<std::string, Position>& loaded,
        std::unordered_map<std::string, Position>& open,
        std::unordered_map<std::string, Position>& closed,
        double tol = kRowTolerance) {
        open.clear();
        closed.clear();
        for (const auto& [symbol, position] : loaded) {
            if (std::abs(position.quantity.as_double()) <= tol) {
                closed[symbol] = position;
            } else {
                open[symbol] = position;
            }
        }
    }

    /**
     * @brief Put the LOADED T-1 realized figure back on each finalized T-1 row.
     *
     * History: LivePnLManager::finalize_previous_day used to write the settlement
     * move qty x (close(T-1) - close(T-2)) into every finalized row's realized_pnl
     * regardless of policy. Under SETTLED that is the day's realized; under
     * MARK_TO_MARKET it is a mark, and writing it over the trade realized the day's
     * own run recorded is how the column came to hold price moves (three times over
     * a weekend). Since R-1 (af1bf2c6) the finalizer itself keeps the row's realized
     * under MARK_TO_MARKET, so this helper is belt and braces on that path.
     *
     * It still matters for one case: select_finalization_book may finalize a symbol
     * from the RESTATED book (a deferred class-1 event covering T-1, E2-F16), and the
     * restated entry's realized is not what T-1's own run wrote. Restoring from the
     * pre-action loaded snapshot keeps the T-1 row's realized equal to the figure
     * the day itself persisted. The finalizer's aggregate fields --
     * finalized_daily_pnl, position_realized_pnl, finalized_unrealized_pnl -- are
     * not touched, so yesterday_total_pnl and total_unrealized_pnl stay as they are.
     *
     * A finalized row with no loaded counterpart cannot carry a loaded realized; it
     * gets 0 rather than the mark move.
     */
    static void restore_loaded_realized(
        std::vector<Position>& finalized,
        const std::unordered_map<std::string, Position>& loaded) {
        for (auto& position : finalized) {
            auto it = loaded.find(position.symbol);
            position.realized_pnl =
                it != loaded.end() ? it->second.realized_pnl : Decimal(0.0);
        }
    }

    /**
     * @brief Give a row to a symbol the strategy realized P&L on that has no
     *        entry in the day-T book.
     *
     * The day-T book is the strategy's target map, which covers the configured
     * universe. A held symbol that has LEFT the universe -- contra-merged, renamed
     * to a name not in config, or simply de-configured -- is still closed out by
     * ExecutionManager::generate_daily_executions and still booked by
     * on_execution(), so its realized reaches the aggregate; but nothing iterates
     * it into a row, so the rows no longer sum to the aggregate. This is the case
     * the in-run L5 assertion would otherwise trip on.
     *
     * @return the symbols given a row, sorted, for the caller to log.
     */
    static std::vector<std::string> add_rowless_exits(
        std::unordered_map<std::string, Position>& positions,
        const std::unordered_map<std::string, Position>& strategy_positions,
        const Timestamp& now,
        double tol = kRowTolerance) {
        std::vector<std::string> added;
        for (const auto& [symbol, held] : strategy_positions) {
            if (positions.find(symbol) != positions.end()) continue;
            if (std::abs(held.realized_pnl.as_double()) <= tol) continue;
            Position closed;
            closed.symbol = symbol;
            closed.quantity = Quantity(0.0);
            closed.average_price = Decimal(0.0);  // no basis: the position no longer exists
            closed.unrealized_pnl = Decimal(0.0);
            closed.realized_pnl = held.realized_pnl;
            closed.last_update = now;
            positions[symbol] = closed;
            added.push_back(symbol);
        }
        std::sort(added.begin(), added.end());
        return added;
    }

    /**
     * @brief The book the Day T-1 finalization should be run from, per symbol.
     *
     * T-1 is finalized as the book actually stood on T-1, i.e. from the snapshot
     * taken before any corporate action touched it (8a1a96ef). The exception is a
     * DEFERRED class-1 event catching up: when its ex-date is on or before T-1 the
     * T-1 close is already post-event, so the pre-action basis would be a frame
     * behind the price (E2-F16); that symbol is finalized from the restated book.
     *
     * The restated book is the one taken AFTER the class-1 rescale and BEFORE the
     * lifecycle handlers. Terminations set quantity to 0 and add a day-T cash flow
     * to realized_pnl; renames and contra-merges re-key and merge entries. None of
     * that happened on T-1, and reading the post-lifecycle map here would finalize
     * a symbol that split and terminated in the same run as a qty-0 row with a
     * day-T flow on the T-1 date (E2-F19 gap G4).
     *
     * @param restated_out symbols taken from the restated book, for the caller's log.
     */
    static std::vector<Position> select_finalization_book(
        const std::unordered_map<std::string, Position>& pre_action,
        const std::unordered_map<std::string, Position>& post_class1,
        const std::unordered_map<std::string, std::string>& applied_class1_ex_date,
        const std::string& t1_date,
        std::vector<std::string>* restated_out = nullptr) {
        std::vector<Position> book;
        book.reserve(pre_action.size());
        for (const auto& [symbol, position] : pre_action) {
            auto ex = applied_class1_ex_date.find(symbol);
            const bool event_covers_t1 =
                ex != applied_class1_ex_date.end() && ex->second <= t1_date;
            if (event_covers_t1) {
                auto restated = post_class1.find(symbol);
                if (restated != post_class1.end()) {
                    book.push_back(restated->second);
                    if (restated_out) restated_out->push_back(symbol);
                    continue;
                }
            }
            book.push_back(position);
        }
        return book;
    }
};

}  // namespace trade_ngin
