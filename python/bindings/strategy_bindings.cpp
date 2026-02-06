#include <pybind11/pybind11.h>
#include "trade_ngin/strategy/strategy_interface.hpp"

namespace py = pybind11;

struct PyStrategyInterface : trade_ngin::StrategyInterface {
    using trade_ngin::StrategyInterface::StrategyInterface;  // Inherit constructors

    // virtual ~StrategyInterface() = default;
    //
    // // Core strategy operations
    // virtual Result<void> initialize() = 0;
    // virtual Result<void> start() = 0;
    // virtual Result<void> stop() = 0;
    // virtual Result<void> pause() = 0;
    // virtual Result<void> resume() = 0;
    //
    // // Data processing
    // virtual Result<void> on_data(const std::vector<Bar>& data) = 0;
    // virtual Result<void> on_execution(const ExecutionReport& report) = 0;
    // virtual Result<void> on_signal(const std::string& symbol, double signal) = 0;
    //
    // // State and metrics
    // virtual StrategyState get_state() const = 0;
    // virtual const StrategyMetrics& get_metrics() const = 0;
    // virtual const StrategyConfig& get_config() const = 0;
    // virtual const StrategyMetadata& get_metadata() const = 0;
    // virtual std::unordered_map<std::string, std::vector<double>> get_price_history() const = 0;
    // // Position management
    // virtual const std::unordered_map<std::string, Position>& get_positions() const = 0;
    // virtual Result<void> update_position(const std::string& symbol, const Position& position) =
    // 0;
    //
    // /**
    //  * @brief Get target positions for portfolio allocation
    //  * @note Override in derived classes that calculate positions differently
    //  *       (e.g., trend-following strategies using instrument_data_)
    //  * @return Map of positions by symbol (copy, not reference)
    //  */
    // virtual std::unordered_map<std::string, Position> get_target_positions() const = 0;
    //
    // // Risk management
    // virtual Result<void> update_risk_limits(const RiskLimits& limits) = 0;
    // virtual Result<void> check_risk_limits() = 0;
    //
    // /**
    //  * @brief Set backtest mode for this strategy
    //  * @param is_backtest True if running in backtest mode (stores daily PnL), false for live
    //  * (cumulative PnL)
    //  * @note Default implementation does nothing. Override in BaseStrategy for backtest-specific
    //  * behavior. In backtest mode, realized_pnl stores DAILY PnL for correct equity curve
    //  * accumulation. In live mode, realized_pnl stores CUMULATIVE PnL for compatibility with
    //  * existing systems.
    //  */
    // virtual void set_backtest_mode(bool /*is_backtest*/) {}
    // virtual bool is_backtest_mode() const {
    //     return false;
    // }
};
