#include "trade_ngin/strategy/strategy_manager.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

StrategyManager::StrategyManager(AllocationConfig config) : config_(std::move(config)) {}

Result<void> StrategyManager::add_strategy(
    std::shared_ptr<StrategyInterface> strategy,
    double initial_allocation) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    try {
        if (!strategy) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Strategy cannot be null",
                "StrategyManager"
            );
        }

        const auto& metadata = strategy->get_metadata();

        // Check if strategy already exists
        if (strategies_.find(metadata.id) != strategies_.end()) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Strategy already exists: " + metadata.id,
                "StrategyManager"
            );
        }

        // Validate allocation
        if (initial_allocation < config_.min_strategy_allocation ||
            initial_allocation > config_.max_strategy_allocation) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Initial allocation out of bounds",
                "StrategyManager"
            );
        }

        // Calculate total allocation after adding new strategy
        double total_allocation = initial_allocation;
        for (const auto& [_, info] : strategies_) {
            total_allocation += info.allocation;
        }

        if (total_allocation > 1.0) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Total allocation would exceed 1.0",
                "StrategyManager"
            );
        }

        // Initialize strategy info
        StrategyInfo info{
            strategy,
            initial_allocation,
            0.0 // current_capital will be set during capital allocation
        };

        // Calculate initial capital allocation
        double capital_available = config_.total_capital - config_.reserve_capital;
        info.current_capital = capital_available * initial_allocation;

        strategies_[metadata.id] = std::move(info);

        INFO("Added strategy " + metadata.id + " with allocation " +
             std::to_string(initial_allocation));

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error adding strategy: ") + e.what(),
            "StrategyManager"
        );
    }
}

Result<void> StrategyManager::remove_strategy(const std::string& strategy_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = strategies_.find(strategy_id);
    if (it == strategies_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Strategy not found: " + strategy_id,
            "StrategyManager"
        );
    }

    // Stop the strategy before removing
    auto stop_result = it->second.strategy->stop();
    if (stop_result.is_error()) {
        WARN("Error stopping strategy: " + std::string(stop_result.error()->what()));
    }

    strategies_.erase(it);
    INFO("Removed strategy: " + strategy_id);

    return Result<void>();
}

Result<void> StrategyManager::start_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [id, info] : strategies_) {
        auto result = info.strategy->start();
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Failed to start strategy " + id + ": " + result.error()->what(),
                "StrategyManager"
            );
        }
    }

    return Result<void>();
}

Result<void> StrategyManager::stop_all() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [id, info] : strategies_) {
        auto result = info.strategy->stop();
        if (result.is_error()) {
            WARN("Error stopping strategy " + id + ": " + result.error()->what());
        }
    }

    return Result<void>();
}

Result<void> StrategyManager::update_allocations(
    const std::unordered_map<std::string, double>& allocations) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        // Validate allocations
        double total = 0.0;
        for (const auto& [id, allocation] : allocations) {
            if (strategies_.find(id) == strategies_.end()) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Strategy not found: " + id,
                    "StrategyManager"
                );
            }

            if (allocation < config_.min_strategy_allocation ||
                allocation > config_.max_strategy_allocation) {
                return make_error<void>(
                    ErrorCode::INVALID_ARGUMENT,
                    "Invalid allocation for strategy " + id,
                    "StrategyManager"
                );
            }

            total += allocation;
        }

        if (std::abs(total - 1.0) > 1e-6) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Allocations must sum to 1.0",
                "StrategyManager"
            );
        }

        // Update allocations and adjust positions
        double capital_available = config_.total_capital - config_.reserve_capital;
        
        for (const auto& [id, allocation] : allocations) {
            auto& info = strategies_[id];
            
            // Calculate capital adjustment
            double new_capital = capital_available * allocation;
            double capital_ratio = new_capital / info.current_capital;
            
            // Adjust positions based on new capital
            auto result = adjust_positions(id, info.allocation, allocation);
            if (result.is_error()) {
                return result;
            }
            
            // Update strategy info
            info.allocation = allocation;
            info.current_capital = new_capital;
        }

        return Result<void>();

    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error updating allocations: ") + e.what(),
            "StrategyManager"
        );
    }
}

Result<void> StrategyManager::process_data(const std::vector<Bar>& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [id, info] : strategies_) {
        auto result = info.strategy->on_data(data);
        if (result.is_error()) {
            return make_error<void>(
                result.error()->code(),
                "Error processing data for strategy " + id + ": " + 
                result.error()->what(),
                "StrategyManager"
            );
        }
    }

    return Result<void>();
}

std::unordered_map<std::string, Position> StrategyManager::get_aggregate_positions() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, Position> aggregate_positions;

    for (const auto& [_, info] : strategies_) {
        const auto& strategy_positions = info.strategy->get_positions();
        
        for (const auto& [symbol, pos] : strategy_positions) {
            if (aggregate_positions.count(symbol) == 0) {
                aggregate_positions[symbol] = pos;
                aggregate_positions[symbol].quantity *= info.allocation;
            } else {
                aggregate_positions[symbol].quantity += pos.quantity * info.allocation;
                
                // Update average price as weighted average
                double total_qty = aggregate_positions[symbol].quantity;
                if (total_qty != 0.0) {
                    aggregate_positions[symbol].average_price = 
                        (aggregate_positions[symbol].average_price * 
                         (total_qty - pos.quantity * info.allocation) +
                         pos.average_price * pos.quantity * info.allocation) / total_qty;
                }
            }
        }
    }

    return aggregate_positions;
}

std::unordered_map<std::string, StrategyMetrics> StrategyManager::get_all_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, StrategyMetrics> all_metrics;
    for (const auto& [id, info] : strategies_) {
        all_metrics[id] = info.strategy->get_metrics();
    }

    return all_metrics;
}

Result<void> StrategyManager::adjust_positions(
    const std::string& strategy_id,
    double old_allocation,
    double new_allocation) {
    
    try {
        auto& info = strategies_[strategy_id];
        
        // Get current positions
        auto positions = info.strategy->get_positions();
        
        // Calculate scale factor for position adjustment
        double scale = new_allocation / old_allocation;
        
        // Adjust each position
        for (auto& [symbol, pos] : positions) {
            pos.quantity *= scale;
            auto result = info.strategy->update_position(symbol, pos);
            if (result.is_error()) {
                return make_error<void>(
                    result.error()->code(),
                    "Error adjusting position for " + symbol + ": " + 
                    result.error()->what(),
                    "StrategyManager"
                );
            }
        }
        
        return Result<void>();
        
    } catch (const std::exception& e) {
        return make_error<void>(
            ErrorCode::UNKNOWN_ERROR,
            std::string("Error adjusting positions: ") + e.what(),
            "StrategyManager"
        );
    }
}

Result<void> StrategyManager::validate_allocations(
    const std::unordered_map<std::string, double>& allocations) const {
    
    double total = 0.0;
    
    for (const auto& [id, allocation] : allocations) {
        if (strategies_.find(id) == strategies_.end()) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Strategy not found: " + id,
                "StrategyManager"
            );
        }
        
        if (allocation < config_.min_strategy_allocation ||
            allocation > config_.max_strategy_allocation) {
            return make_error<void>(
                ErrorCode::INVALID_ARGUMENT,
                "Allocation out of bounds for strategy " + id,
                "StrategyManager"
            );
        }
        
        total += allocation;
    }
    
    if (std::abs(total - 1.0) > 1e-6) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Allocations must sum to 1.0",
            "StrategyManager"
        );
    }
    
    return Result<void>();
}

} // namespace trade_ngin