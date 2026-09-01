#pragma once

#include "trade_ngin/live/pnl_manager_base.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include <memory>
#include <functional>

namespace trade_ngin {

/**
 * Live implementation of PnL Manager
 * Handles Day T-1 finalization and position PnL calculations
 * This extracts ~200+ lines of PnL logic from live_trend.cpp
 */
class LivePnLManager : public PnLManagerBase {
private:
    // PnL tracking
    std::unordered_map<std::string, double> position_daily_pnl_;
    std::unordered_map<std::string, double> position_realized_pnl_;
    std::unordered_map<std::string, double> position_unrealized_pnl_;

    double cumulative_daily_pnl_ = 0.0;
    double cumulative_total_pnl_ = 0.0;

    // Reference to instrument registry for point values
    InstrumentRegistry& registry_;

public:
    /**
     * Constructor
     * @param initial_capital Starting capital
     * @param registry Reference to instrument registry
     */
    explicit LivePnLManager(double initial_capital, InstrumentRegistry& registry)
        : PnLManagerBase(initial_capital), registry_(registry) {}

    /**
     * @brief How Day T-1 finalization records unrealized P&L.
     *
     * SETTLED (the default) -- futures. Under the T-1 lag model the runner enters a
     * position at close(T-1) and the next day's run settles it at close(T), booking the
     * entire move as realized. The position is never carried at an unsettled price, so
     * there is no unrealized component: 0 is an identity of the model, not a convention.
     *
     * This matters because `average_price` on a futures row IS the prior settlement
     * close, i.e. exactly the `t2_close` the finalizer already uses for realized. So
     * `quantity * (t1_close - average_price) * point_value` is algebraically the realized
     * formula, and writing it into unrealized records the same settlement move in two
     * columns -- E2-F3, measured at up to -12,586.69 in a day, and a violation of the
     * row-sums-to-aggregate invariant since live_results correctly reports 0.
     *
     * MARK_TO_MARKET -- equities. There `average_price` is a true weighted cost basis, so
     * the same expression is the genuine unrealized P&L.
     *
     * The default is SETTLED so that a caller which says nothing keeps the futures
     * behaviour; both futures runners pass no policy argument.
     */
    enum class UnrealizedPolicy { SETTLED, MARK_TO_MARKET };

    /**
     * @brief Unrealized P&L of one position against its own cost basis.
     *
     * The single rule for "what is this position worth versus what it cost", shared by
     * the live_results aggregate this manager produces and the per-row
     * trading.positions.unrealized_pnl the equity runner persists. It is a named
     * function because those two used to disagree: the aggregate was computed from
     * Position::average_price while the persisted row read
     * MeanReversionInstrumentData::entry_price, a field with no writer anywhere in the
     * tree. It was therefore always 0.0, so every persisted row said 0 while the
     * aggregate said otherwise -- a guaranteed log-versus-DB mismatch on any day with
     * open positions.
     *
     * Returns 0.0 when there is no basis to measure against: a zero quantity, or an
     * average_price not yet set (on_execution is its sole writer, so a fill that has not
     * been processed yet leaves it at 0).
     *
     * It deliberately does NOT screen mark_price. That is the caller's business and the
     * two callers differ: this manager skips a position whose price is absent, while the
     * equity runner defaults a missing close to 0.0 and must check it itself. Folding an
     * `mark_price > 0` guard in here would have quietly changed what the futures path
     * reports for a present-but-zero mark, which is outside this fix's scope.
     */
    static double unrealized_from_cost_basis(double quantity, double average_price,
                                             double mark_price, double point_value = 1.0) {
        if (quantity == 0.0 || average_price <= 0.0) return 0.0;
        return quantity * (mark_price - average_price) * point_value;
    }

    /**
     * @brief The cost basis to record against a position on day T.
     *
     * A basis is established when a position is opened and has to survive every
     * day it is held: realized PnL on exit, unrealized PnL while held, and the
     * mean-reversion stop-loss are all measured from it. The equity runner used to
     * seed every day-T position's average_price with the previous session's close
     * and then correct only the symbols the strategy knew about -- which, because
     * positions_ was never seeded, meant only the symbols that traded that day. A
     * held-but-untraded position therefore had its basis re-anchored to the latest
     * close every session: it always looked flat, its unrealized PnL was always
     * ~0, and its stop-loss measured from yesterday rather than from what it cost.
     *
     * @param strategy_basis The weighted average BaseStrategy::on_execution()
     *        maintains. Authoritative when set: a symbol that traded today has a
     *        basis that already accounts for today's fills. <= 0 when the strategy
     *        has no record of the symbol.
     * @param carried_basis The basis the same symbol carries in the previous day's
     *        book, AFTER corporate actions have restated it. <= 0 when the symbol
     *        is not a carried-over holding.
     * @return the basis to record, or 0.0 when neither source knows one. A mark is
     *         deliberately never a candidate: the day's close is what the position is
     *         worth, not what it cost, and substituting one for the other is the bug
     *         above.
     *
     * On the 0.0 return: it means "no basis is known", and it is the CALLER's job to
     * decide what that means for a row it is about to persist. Do not read it as a
     * price and do not skip the write and leave the day-T placeholder in place -- that
     * placeholder is the previous close, so skipping reinstates the very mark-as-basis
     * substitution this function exists to prevent. The equity runner treats it as an
     * upstream invariant failure: it logs the symbol and records a zero basis with zero
     * unrealized PnL, so the row reads as incomplete rather than as a plausible lie.
     *
     * In normal flow it should be unreachable for a held position. Every holding has a
     * basis -- it either traded today, in which case on_execution() set one from a fill
     * that ExecutionManager refused to price without a real close, or it was carried,
     * in which case the seeded book supplied one.
     */
    static double resolve_day_t_cost_basis(double strategy_basis, double carried_basis) {
        if (strategy_basis > 0.0) return strategy_basis;
        if (carried_basis > 0.0) return carried_basis;
        return 0.0;
    }

    /**
     * Finalization result structure for Day T-1
     */
    struct FinalizationResult {
        double finalized_daily_pnl = 0.0;
        double finalized_portfolio_value = 0.0;
        std::unordered_map<std::string, double> position_realized_pnl;
        std::vector<Position> finalized_positions;
        bool success = false;
    };

    /**
     * Finalize previous day (T-1) positions
     * This is unique to live trading and handles the settlement process
     * Replaces the finalization logic in live_trend.cpp (lines ~600-700)
     */
    Result<FinalizationResult> finalize_previous_day(
        const std::vector<Position>& previous_positions,
        const std::unordered_map<std::string, double>& t1_close_prices,
        const std::unordered_map<std::string, double>& t2_close_prices,
        double previous_portfolio_value,
        double commissions = 0.0,
        UnrealizedPolicy unrealized_policy = UnrealizedPolicy::SETTLED);

    /**
     * Calculate PnL for current day positions
     * Replaces the position PnL calculation logic in live_trend.cpp
     */
    Result<void> calculate_position_pnls(
        const std::vector<Position>& positions,
        const std::unordered_map<std::string, double>& current_prices,
        const std::unordered_map<std::string, double>& previous_prices);

    /**
     * Update position PnL
     */
    Result<void> update_position_pnl(
        const std::string& symbol,
        double daily_pnl,
        double realized_pnl = 0.0);

    /**
     * Get current PnL snapshot
     */
    Result<PnLSnapshot> get_current_snapshot() const;

    /**
     * Get daily PnL for a specific symbol
     */
    double get_position_daily_pnl(const std::string& symbol) const;

    /**
     * Get realized PnL for a specific symbol
     */
    double get_position_realized_pnl(const std::string& symbol) const;

    /**
     * Get total daily PnL across all positions
     */
    double get_total_daily_pnl() const {
        return cumulative_daily_pnl_;
    }

    /**
     * Get total cumulative PnL
     */
    double get_total_pnl() const {
        return cumulative_total_pnl_;
    }

    /**
     * Clear all PnL tracking for new day
     */
    void reset_daily_tracking() {
        position_daily_pnl_.clear();
        position_realized_pnl_.clear();
        position_unrealized_pnl_.clear();
        cumulative_daily_pnl_ = 0.0;
    }

    /**
     * Set cumulative total PnL (for initialization)
     */
    void set_total_pnl(double total_pnl) {
        cumulative_total_pnl_ = total_pnl;
    }

    /**
     * Helper to get point value for a symbol
     * Uses InstrumentRegistry with accurate fallback values
     */
    double get_point_value(const std::string& symbol) const;

private:
    /**
     * Get fallback multiplier for known symbols
     * These are calculated as: minimum_price_fluctuation / tick_size
     */
    double get_fallback_multiplier(const std::string& symbol) const;
};

} // namespace trade_ngin