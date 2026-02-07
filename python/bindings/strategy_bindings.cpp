#include <pybind11/pybind11.h>

#include "trade_ngin/strategy/strategy_interface.hpp"

namespace py = pybind11;

using namespace trade_ngin;

struct PyStrategyInterface : trade_ngin::StrategyInterface {
    using trade_ngin::StrategyInterface::StrategyInterface;  // Inherit constructors

    // Core strategy operations
    Result<void> initialize() override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, initialize);
    };
    Result<void> start() override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, start);
    };
    Result<void> stop() override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, stop);
    };
    Result<void> pause() override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, pause);
    };
    Result<void> resume() override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, resume);
    };

    // Data processing
    Result<void> on_data(const std::vector<Bar>& data) override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, on_data, data);
    };
    Result<void> on_execution(const ExecutionReport& report) override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, on_execution, report);
    };
    Result<void> on_signal(const std::string& symbol, double signal) override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, onsignal, symbol, signal);
    };

    // State and metrics
    StrategyState get_state() const override {
        PYBIND11_OVERRIDE_PURE(StrategyState, StrategyInterface, get_state);
    };
    // TODO need to find solution for these: either change to not using a reference which would
    // require changing the source code, or some other workaround const StrategyMetrics&
    //
    // get_metrics() const override {
    //     PYBIND11_OVERRIDE_PURE(StrategyMetrics&, StrategyInterface, get_metrics);
    // };
    // const StrategyConfig& get_config() const override {
    //     PYBIND11_OVERRIDE_PURE(StrategyConfig&, StrategyInterface, get_config);
    // };
    // const StrategyMetadata& get_metadata() const override {
    //     PYBIND11_OVERRIDE_PURE(StrategyMetadata&, StrategyInterface, get_metadata);
    // };
    // TODO unsure what to do because the LSP is saying that unordered_map doesn't have enough
    // template arguments? std::unordered_map<std::string, std::vector<double>> get_price_history()
    // const override {
    //     PYBIND11_OVERRIDE_PURE(std::unordered_map<std::string, std::vector<double>>,
    //                            StrategyInterface, get_price_history);
    // }
    // Position management
    // const std::unordered_map<std::string, Position>& get_positions() const override {
    //     // PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, get_positions);
    // };
    Result<void> update_position(const std::string& symbol, const Position& position) override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, update_position, symbol, position);
    };

    /**
     * @brief Get target positions for portfolio allocation
     * @note Override in derived classes that calculate positions differently
     *       (e.g., trend-following strategies using instrument_data_)
     * @return Map of positions by symbol (copy, not reference)
     */
    std::unordered_map<std::string, Position> get_target_positions() const override {
        // PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, get_target_positions);
    };

    // Risk management
    Result<void> update_risk_limits(const RiskLimits& limits) override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, update_risk_limits, limits);
    };
    Result<void> check_risk_limits() override {
        PYBIND11_OVERRIDE_PURE(Result<void>, StrategyInterface, check_risk_limits);
    };

    /**
     * @brief Set backtest mode for this strategy
     * @param is_backtest True if running in backtest mode (stores daily PnL), false for live
     * (cumulative PnL)
     * @note Default implementation does nothing. Override in BaseStrategy for backtest-specific
     * behavior. In backtest mode, realized_pnl stores DAILY PnL for correct equity curve
     * accumulation. In live mode, realized_pnl stores CUMULATIVE PnL for compatibility with
     * existing systems.
     */
    // void set_backtest_mode(bool /*is_backtest*/) {}
    // bool is_backtest_mode() const {
    //     return false;
    // }
};
