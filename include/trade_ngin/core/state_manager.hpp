//===== state_manager.hpp =====
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

enum class ComponentState { INITIALIZED, RUNNING, PAUSED, ERR_STATE, STOPPED };

enum class ComponentType {
    STRATEGY,
    OPTIMIZER,
    RISK_MANAGER,
    PORTFOLIO_MANAGER,
    MARKET_DATA,
    ORDER_MANAGER,
    DATABASE,
    EXECUTION_ENGINE,
    BACKTEST_ENGINE
};

struct ComponentInfo {
    ComponentType type;
    ComponentState state;
    std::string id;
    std::string error_message;
    Timestamp last_update;
    std::unordered_map<std::string, double> metrics;
};

class StateManager {
public:
    static StateManager& instance() {
        static StateManager instance;
        return instance;
    }

    Result<ComponentInfo> get_state(const std::string& component_id) const;
    Result<void> update_metrics(const std::string& component_id,
                                const std::unordered_map<std::string, double>& metrics);
    Result<void> register_component(const ComponentInfo& info);
    Result<void> unregister_component(const std::string& component_id);
    Result<void> update_state(const std::string& component_id, ComponentState new_state,
                              const std::string& error_message = "");
    bool is_healthy() const;
    std::vector<std::string> get_all_components() const;

    static void reset_instance() {
        auto& inst = instance();
        std::unique_lock<std::recursive_mutex> lock(inst.mutex_);
        inst.components_.clear();
        inst.cv_.notify_all();
    }

    void shutdown();

private:
    StateManager() = default;
    StateManager(const StateManager&) = delete;
    StateManager& operator=(const StateManager&) = delete;

    Result<void> validate_transition(ComponentState current_state, ComponentState new_state) const;

    std::unordered_map<std::string, ComponentInfo> components_;
    mutable std::recursive_mutex mutex_;
    std::condition_variable_any cv_;

    std::chrono::steady_clock::time_point last_reset_;
};
}  // namespace trade_ngin