// include/trade_ngin/strategy/base_strategy.hpp
#pragma once

#include <atomic>
#include <mutex>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/types.hpp"

namespace trade_ngin {

/**
 * @brief Base class for all trading strategies
 */
class BaseStrategy : public StrategyInterface {
public:
    /**
     * @brief Constructor
     * @param id Strategy identifier
     * @param config Strategy configuration
     * @param db Database interface
     */
    BaseStrategy(std::string id, StrategyConfig config, std::shared_ptr<PostgresDatabase> db);

    /**
     * @brief Destructor
     */
    virtual ~BaseStrategy() = default;

    // StrategyInterface implementations
    /**
     * @brief Initialize the strategy
     * @return Result indicating success or failure
     */
    Result<void> initialize() override;

    /**
     * @brief Start the strategy
     * @return Result indicating success or failure
     */
    Result<void> start() override;

    /**
     * @brief Stop the strategy
     * @return Result indicating success or failure
     */
    Result<void> stop() override;

    /**
     * @brief Pause the strategy
     * @return Result indicating success or failure
     */
    Result<void> pause() override;

    /**
     * @brief Resume the strategy
     * @return Result indicating success or failure
     */
    Result<void> resume() override;

    /**
     * @brief Process new market data
     * @param data Vector of price bars
     * @return Result indicating success or failure
     */
    Result<void> on_data(const std::vector<Bar>& data) override;

    /**
     * @brief Process execution reports
     * @param report Execution report
     * @return Result indicating success or failure
     */
    Result<void> on_execution(const ExecutionReport& report) override;

    /**
     * @brief Process signals from external sources
     * @param symbol Symbol for the signal
     * @param signal Signal value
     * @return Result indicating success or failure
     */
    Result<void> on_signal(const std::string& symbol, double signal) override;

    /**
     * @brief Get the current state of the strategy
     * @return Strategy state
     */
    StrategyState get_state() const override;

    /**
     * @brief Get the current metrics for the strategy
     * @return Strategy metrics
     */
    const StrategyMetrics& get_metrics() const override;

    /**
     * @brief Get the current configuration for the strategy
     * @return Strategy configuration
     */
    const StrategyConfig& get_config() const override;

    /**
     * @brief Get the metadata for the strategy
     * @return Strategy metadata
     */
    const StrategyMetadata& get_metadata() const override;

    /**
     * @brief Get the price history for all symbols
     * @return Price history by symbol
     */
    std::unordered_map<std::string, std::vector<double>> get_price_history() const override;

    /**
     * @brief Get the current positions for the strategy
     * @return Map of positions by symbol
     */
    const std::unordered_map<std::string, Position>& get_positions() const override;

    /**
     * @brief Update a position for a symbol
     * @param symbol Symbol to update
     * @param position New position
     * @return Result indicating success or failure
     */
    Result<void> update_position(const std::string& symbol, const Position& position) override;

    /**
     * @brief Update risk limits for the strategy
     * @param limits New risk limits
     * @return Result indicating success or failure
     */
    Result<void> update_risk_limits(const RiskLimits& limits) override;

    /**
     * @brief Get the current signals for the strategy
     * @return Map of signals by symbol
     */
    Result<void> check_risk_limits() override;

    /**
     * @brief Get the current signals for the strategy
     * @return Map of signals by symbol
     */
    virtual Result<void> update_metrics();

    /**
     * @brief Transition the strategy to a new state
     * @param new_state New state to transition to
     * @return Result indicating success or failure
     */
    Result<void> transition_state(StrategyState new_state);

protected:
    // Protected methods for derived classes
    virtual Result<void> validate_config() const;
    virtual Result<void> save_signals(const std::unordered_map<std::string, double>& signals);
    virtual Result<void> save_positions();
    virtual Result<void> save_executions(const ExecutionReport& exec);

    // Data members
    std::string id_;
    StrategyConfig config_;
    StrategyMetadata metadata_;
    StrategyMetrics metrics_;
    std::atomic<StrategyState> state_;

    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, double> last_signals_;
    RiskLimits risk_limits_;

    std::shared_ptr<PostgresDatabase> db_;
    mutable std::mutex mutex_;

private:
    /**
     * @brief Validate a state transition
     * @param new_state New state to transition to
     * @return Result indicating success or failure
     */
    Result<void> validate_state_transition(StrategyState new_state) const;

    std::string registered_component_id_;
    bool is_initialized_{false};
    std::atomic<bool> running_{false};
};

}  // namespace trade_ngin
