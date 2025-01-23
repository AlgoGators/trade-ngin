// include/trade_ngin/strategy/base_strategy.hpp
#pragma once

#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/strategy/types.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include <atomic>
#include <mutex>

namespace trade_ngin {

class BaseStrategy : public StrategyInterface {
public:
    BaseStrategy(std::string id,
                StrategyConfig config,
                std::shared_ptr<DatabaseInterface> db);
    
    virtual ~BaseStrategy() = default;

    // StrategyInterface implementations
    Result<void> initialize() override;
    Result<void> start() override;
    Result<void> stop() override;
    Result<void> pause() override;
    Result<void> resume() override;

    Result<void> on_data(const std::vector<Bar>& data) override;
    Result<void> on_execution(const ExecutionReport& report) override;
    Result<void> on_signal(const std::string& symbol, double signal) override;
    
    StrategyState get_state() const override;
    const StrategyMetrics& get_metrics() const override;
    const StrategyConfig& get_config() const override;
    const StrategyMetadata& get_metadata() const override;
    
    const std::unordered_map<std::string, Position>& get_positions() const override;
    Result<void> update_position(const std::string& symbol, 
                               const Position& position) override;
    
    Result<void> update_risk_limits(const RiskLimits& limits) override;
    Result<void> check_risk_limits() override;

protected:
    // Protected methods for derived classes
    virtual Result<void> validate_config() const;
    virtual Result<void> save_signals(const std::unordered_map<std::string, double>& signals);
    virtual Result<void> save_positions();
    virtual Result<void> save_executions(const ExecutionReport& exec);
    virtual Result<void> update_metrics();
    
    // Data members
    std::string id_;
    StrategyConfig config_;
    StrategyMetadata metadata_;
    StrategyMetrics metrics_;
    std::atomic<StrategyState> state_;
    
    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, double> last_signals_;
    RiskLimits risk_limits_;
    
    std::shared_ptr<DatabaseInterface> db_;
    mutable std::mutex mutex_;

private:
    Result<void> transition_state(StrategyState new_state);
    Result<void> validate_state_transition(StrategyState new_state) const;
};

} // namespace trade_ngin