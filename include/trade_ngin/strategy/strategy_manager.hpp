// include/trade_ngin/strategy/strategy_manager.hpp
#pragma once

#include "trade_ngin/strategy/strategy_interface.hpp"
#include "trade_ngin/core/error.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>

namespace trade_ngin {

/**
 * @brief Configuration for strategy allocation
 */
struct AllocationConfig {
    double total_capital;            // Total capital to allocate
    double max_strategy_allocation;  // Maximum allocation to any strategy
    double min_strategy_allocation;  // Minimum allocation to any strategy
    double reserve_capital;          // Capital to keep in reserve
    bool dynamic_allocation;         // Whether to use dynamic allocation
};

/**
 * @brief Manages multiple strategies and their allocations
 */
class StrategyManager {
public:
    explicit StrategyManager(AllocationConfig config);

    /**
     * @brief Add a strategy to the manager
     * @param strategy Strategy to add
     * @param initial_allocation Initial capital allocation
     * @return Result indicating success or failure
     */
    Result<void> add_strategy(std::shared_ptr<StrategyInterface> strategy,
                            double initial_allocation);

    /**
     * @brief Remove a strategy from the manager
     * @param strategy_id ID of strategy to remove
     * @return Result indicating success or failure
     */
    Result<void> remove_strategy(const std::string& strategy_id);

    /**
     * @brief Start all strategies
     * @return Result indicating success or failure
     */
    Result<void> start_all();

    /**
     * @brief Stop all strategies
     * @return Result indicating success or failure
     */
    Result<void> stop_all();

    /**
     * @brief Update strategy allocations
     * @param allocations Map of strategy ID to new allocation
     * @return Result indicating success or failure
     */
    Result<void> update_allocations(
        const std::unordered_map<std::string, double>& allocations);

    /**
     * @brief Process market data
     * @param data New market data
     * @return Result indicating success or failure
     */
    Result<void> process_data(const std::vector<Bar>& data);

    /**
     * @brief Get aggregated positions across all strategies
     * @return Map of symbol to aggregated position
     */
    std::unordered_map<std::string, Position> get_aggregate_positions() const;

    /**
     * @brief Get strategy metrics
     * @return Map of strategy ID to metrics
     */
    std::unordered_map<std::string, StrategyMetrics> get_all_metrics() const;

private:
    struct StrategyInfo {
        std::shared_ptr<StrategyInterface> strategy;
        double allocation;
        double current_capital;
    };

    Result<void> validate_allocations(
        const std::unordered_map<std::string, double>& allocations) const;
    Result<void> adjust_positions(const std::string& strategy_id,
                                double old_allocation,
                                double new_allocation);

    AllocationConfig config_;
    std::unordered_map<std::string, StrategyInfo> strategies_;
    mutable std::mutex mutex_;
};

} // namespace trade_ngin