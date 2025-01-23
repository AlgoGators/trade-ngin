// include/trade_ngin/core/state_manager.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/core/error.hpp"
#include <memory>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief System component states
 */
enum class ComponentState {
    INITIALIZED,
    RUNNING,
    PAUSED,
    ERROR,
    STOPPED
};

/**
 * @brief System component types
 */
enum class ComponentType {
    STRATEGY,
    OPTIMIZER,
    RISK_MANAGER,
    PORTFOLIO_MANAGER,
    MARKET_DATA,
    ORDER_MANAGER
};

/**
 * @brief Component state info
 */
struct ComponentInfo {
    ComponentType type;
    ComponentState state;
    std::string id;
    std::string error_message;
    Timestamp last_update;
    std::unordered_map<std::string, double> metrics;
};

/**
 * @brief Manager for system state and transitions
 */
class StateManager {
public:
    /**
     * @brief Update component state
     * @param component_id Component identifier
     * @param new_state New state for component
     * @param error_message Optional error message
     * @return Result indicating success or failure
     */
    Result<void> update_state(
        const std::string& component_id,
        ComponentState new_state,
        const std::string& error_message = "");

    /**
     * @brief Get component state
     * @param component_id Component identifier
     * @return Current state info
     */
    Result<ComponentInfo> get_state(const std::string& component_id) const;

    /**
     * @brief Register new component
     * @param info Component information
     * @return Result indicating success or failure
     */
    Result<void> register_component(const ComponentInfo& info);

    /**
     * @brief Update component metrics
     * @param component_id Component identifier
     * @param metrics Map of metric names to values
     * @return Result indicating success or failure
     */
    Result<void> update_metrics(
        const std::string& component_id,
        const std::unordered_map<std::string, double>& metrics);

    /**
     * @brief Check if system is in healthy state
     * @return True if all components are running normally
     */
    bool is_healthy() const;

    /**
     * @brief Get singleton instance
     */
    static StateManager& instance() {
        static StateManager instance;
        return instance;
    }

private:
    StateManager() = default;
    
    std::unordered_map<std::string, ComponentInfo> components_;
    mutable std::mutex mutex_;

    /**
     * @brief Validate state transition
     * @param current_state Current component state
     * @param new_state Requested new state
     * @return Result indicating if transition is valid
     */
    Result<void> validate_transition(
        ComponentState current_state,
        ComponentState new_state) const;
};

} // namespace trade_ngin