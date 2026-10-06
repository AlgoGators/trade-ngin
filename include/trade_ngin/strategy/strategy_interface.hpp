// include/trade_ngin/strategy/strategy_interface.hpp
#pragma once

#include <memory>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/types.hpp"

namespace trade_ngin {

/**
 * @brief Interface for all trading strategies
 */
class StrategyInterface {
public:
    virtual ~StrategyInterface() = default;

    // Core strategy operations
    virtual Result<void> initialize() = 0;
    virtual Result<void> start() = 0;
    virtual Result<void> stop() = 0;
    virtual Result<void> pause() = 0;
    virtual Result<void> resume() = 0;

    // Data processing
    virtual Result<void> on_data(const std::vector<Bar>& data) = 0;
    virtual Result<void> on_execution(const ExecutionReport& report) = 0;
    virtual Result<void> on_signal(const std::string& symbol, double signal) = 0;

    // State and metrics
    virtual StrategyState get_state() const = 0;
    virtual const StrategyMetrics& get_metrics() const = 0;
    virtual const StrategyConfig& get_config() const = 0;
    virtual const StrategyMetadata& get_metadata() const = 0;
    virtual std::unordered_map<std::string, std::vector<double>> get_price_history() const = 0;
    // Position management
    virtual const std::unordered_map<std::string, Position>& get_positions() const = 0;
    virtual Result<void> update_position(const std::string& symbol, const Position& position) = 0;

    /**
     * @brief Seed in-memory positions from an external snapshot (typically yesterday's
     *        EOD positions loaded from DB). Required by live strategies whose buffer
     *        anchors on positions_ across process restarts; backtest may treat as no-op.
     */
    virtual Result<void> seed_positions(
        const std::unordered_map<std::string, Position>& positions) = 0;

    /**
     * @brief Get target positions for portfolio allocation
     * @note Override in derived classes that calculate positions differently
     *       (e.g., trend-following strategies using instrument_data_)
     * @return Map of positions by symbol (copy, not reference)
     */
    virtual std::unordered_map<std::string, Position> get_target_positions() const = 0;

    // Risk management
    virtual Result<void> update_risk_limits(const RiskLimits& limits) = 0;
    virtual Result<void> check_risk_limits() = 0;

    /**
     * @brief The capital the strategy sizes on from the next on_data (T-7b-2 9c, HD 2026-09-25:
     *        sizing uses current equity). The PortfolioManager passes the account's equity, marked
     *        at the close of the newest bar the sizing reads, times this strategy's allocation
     *        (PortfolioManager::set_sizing_capital). A strategy that does not implement it refuses,
     *        so a caller that asked for compounding cannot silently keep a constant capital.
     */
    virtual Result<void> set_capital_allocation(double capital) {
        (void)capital;
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "this strategy does not accept a sizing capital", "Strategy");
    }

    /**
     * @brief Seed the strategy's own history with consumed bars that precede the first bar it is
     *        fed, for estimators whose window reaches back before a run's window. Nothing is
     *        published and nothing is sized. A strategy that keeps no such history ignores it.
     */
    virtual Result<void> seed_history(const std::vector<Bar>& /*bars*/) { return Result<void>(); }

    /**
     * @brief Set backtest mode for this strategy
     * @param is_backtest True if running in backtest mode (stores daily PnL), false for live (cumulative PnL)
     * @note Default implementation does nothing. Override in BaseStrategy for backtest-specific behavior.
     *       In backtest mode, realized_pnl stores DAILY PnL for correct equity curve accumulation.
     *       In live mode, realized_pnl stores CUMULATIVE PnL for compatibility with existing systems.
     */
    virtual void set_backtest_mode(bool /*is_backtest*/) {}

    /**
     * @brief Check if strategy is running in backtest mode
     * @return True if in backtest mode, false by default
     */
    virtual bool is_backtest_mode() const { return false; }

    /**
     * @brief Whether the strategy signals `symbol` yet: it has the history it needs to produce a
     *        forecast, so a target it reports for the symbol is its own (T-OPT E-7, ledger
     *        OPT-new-symbol-collapses-min-periods).
     * @return true by default: a strategy without a warm-up signals every symbol it lists. The
     *         trend strategies return false while the symbol's price history is shorter than the
     *         longest EMA window, the same test on_data applies before it computes a forecast.
     * @note The PortfolioManager leaves a symbol that no optimizing strategy signals out of the
     *       optimizer (its covariance and its date intersection), so a contract still warming up
     *       cannot shorten every other symbol's covariance window to its own history.
     */
    virtual bool is_signalling(const std::string& /*symbol*/) const { return true; }
};

}  // namespace trade_ngin