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

    // Risk management
    virtual Result<void> update_risk_limits(const RiskLimits& limits) = 0;
    virtual Result<void> check_risk_limits() = 0;
};

}  // namespace trade_ngin