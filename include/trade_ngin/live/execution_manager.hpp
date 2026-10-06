#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/transaction_cost/transaction_cost_manager.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <chrono>
#include <memory>

namespace trade_ngin {

/**
 * ExecutionManager - Offline execution synthesizer for the live trading flow.
 *
 * Despite "live" in the path, this class is NOT a real broker adapter.
 * It generates synthetic ExecutionReports from position deltas
 * (target - current) priced at T-1 close, and computes commissions /
 * transaction costs via TransactionCostManager. There is no order routing,
 * no broker network call, no partial-fill handling.
 *
 * This is paper-trading by design per the 2026-05-03 audit §7. A real
 * broker adapter (and the broker-reconciliation work in audit §1.13) lands
 * in a separate phase if and when live brokerage integration ships.
 *
 * For historical simulation, see backtest::BacktestExecutionManager.
 *
 * History: extracted from live_trend.cpp lines 717-833 during the Phase 3
 * refactor. The "live" prefix is preserved for path stability; this
 * docstring is the authoritative description of the role.
 */

/**
 * How generate_daily_executions() prices a fill when market_prices has no usable
 * entry for the symbol.
 *
 * The distinction exists because Position::average_price does not mean the same
 * thing in every strategy. Futures trend-following assigns it the latest mark
 * (trend_following.cpp:623, matching REALIZED_ONLY daily settlement), so falling
 * back to it yields a real price. Equity mean reversion maintains it as a weighted
 * cost basis, which for a position opened today is 0.00 until the fill being priced
 * is itself processed -- so the same fallback books the trade at zero.
 */
enum class PricingPolicy {
    /// Fall back to Position::average_price. Correct where that field holds a mark.
    /// This is the historical behaviour and the default, so existing callers are
    /// unaffected.
    MARK_FALLBACK,

    /// Never invent a price: skip the symbol with an ERROR and report it through
    /// unpriced_out. For callers whose average_price is a cost basis.
    STRICT
};

class ExecutionManager {
private:
    // Transaction cost model (single source of truth)
    std::shared_ptr<transaction_cost::TransactionCostManager> cost_manager_;

    // Track previous close prices for volatility calculation
    std::unordered_map<std::string, double> prev_close_prices_;

public:
    /**
     * Constructor with optional TransactionCostManager config
     */
    explicit ExecutionManager(
        const transaction_cost::TransactionCostManager::Config& config =
            transaction_cost::TransactionCostManager::Config())
        : cost_manager_(std::make_shared<transaction_cost::TransactionCostManager>(config)) {}

    /**
     * Generate execution reports for daily position changes
     *
     * @param current_positions Current day's positions
     * @param previous_positions Previous day's positions
     * @param market_prices Market prices (typically T-1 close prices).
     * @param timestamp Execution timestamp
     * @param pricing How to handle a symbol with no usable price in market_prices.
     *
     *        MARK_FALLBACK (default) is the long-standing behaviour and is CORRECT for
     *        futures: TrendFollowingStrategy sets Position::average_price to
     *        price_history.back() -- the latest mark, by design, matching REALIZED_ONLY
     *        daily settlement (trend_following.cpp:623). Falling back to it prices the
     *        fill at a real, one-session-stale close.
     *
     *        STRICT is for callers whose average_price is a weighted COST BASIS rather
     *        than a mark -- equity mean reversion. There, a position opened today has no
     *        basis until the very fill being priced is processed, so the fallback books
     *        the trade at 0.00, which then persists as the new basis. Such a symbol is
     *        skipped with an ERROR and reported through unpriced_out.
     *
     *        The same field means opposite things by asset class; this parameter is the
     *        seam. See docs/AVERAGE_PRICE_LIFECYCLE.md.
     * @param unpriced_out Optional, STRICT only. Receives the symbols skipped for want of
     *        a price. A skip is not a flat position and not a fill -- the caller must
     *        reconcile these before persisting, or the book will silently disagree with
     *        the executions.
     * @return Vector of execution reports
     */
    Result<std::vector<ExecutionReport>> generate_daily_executions(
        const std::unordered_map<std::string, Position>& current_positions,
        const std::unordered_map<std::string, Position>& previous_positions,
        const std::unordered_map<std::string, double>& market_prices,
        const Timestamp& timestamp,
        PricingPolicy pricing = PricingPolicy::MARK_FALLBACK,
        std::vector<std::string>* unpriced_out = nullptr);

    /**
     * Generate a single execution report
     *
     * @param symbol Symbol being traded
     * @param quantity_change Change in position quantity (positive for buy, negative for sell)
     * @param market_price Market price for the symbol
     * @param timestamp Execution timestamp
     * @param exec_sequence Sequence number for unique exec_id
     * @return Single execution report
     */
    ExecutionReport generate_execution(
        const std::string& symbol,
        double quantity_change,
        double market_price,
        const Timestamp& timestamp,
        size_t exec_sequence);

    /**
     * Update market data for TransactionCostManager (ADV and volatility tracking)
     * Call this with daily data before generating executions for accurate cost estimates.
     *
     * @param symbol Symbol to update
     * @param volume Daily volume for the symbol
     * @param close_price Daily close price for the symbol
     */
    void update_market_data(const std::string& symbol, double volume, double close_price);

    /**
     * Generate date string for order IDs
     *
     * @param timestamp Timestamp to convert
     * @return Date string in YYYYMMDD format
     */
    static std::string generate_date_string(const Timestamp& timestamp);

    /**
     * Generate unique execution ID
     *
     * @param symbol Trading symbol
     * @param timestamp Execution timestamp
     * @param sequence Sequence number
     * @return Unique execution ID
     */
    static std::string generate_exec_id(
        const std::string& symbol,
        const Timestamp& timestamp,
        size_t sequence);

    transaction_cost::TransactionCostManager& get_transaction_cost_manager() {
        return *cost_manager_;
    }
};

} // namespace trade_ngin
