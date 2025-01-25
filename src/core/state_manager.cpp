// src/core/state_manager.cpp
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {

Result<void> StateManager::update_state(
    const std::string& component_id,
    ComponentState new_state,
    const std::string& error_message) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component not found: " + component_id,
            "StateManager"
        );
    }

    // Validate state transition
    auto validation = validate_transition(it->second.state, new_state);
    if (validation.is_error()) {
        return validation;
    }

    // Update state
    ComponentState old_state = it->second.state;
    it->second.state = new_state;
    it->second.last_update = std::chrono::system_clock::now();
    
    if (!error_message.empty()) {
        it->second.error_message = error_message;
    }

    INFO("Component " + component_id + " state changed from " +
         std::to_string(static_cast<int>(old_state)) + " to " +
         std::to_string(static_cast<int>(new_state)));

    if (new_state == ComponentState::ERROR) {
        ERROR("Component " + component_id + " error: " + error_message);
    }

    return Result<void>();  // Success case for void
}

Result<ComponentInfo> StateManager::get_state(
    const std::string& component_id) const {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return make_error<ComponentInfo>(
            ErrorCode::INVALID_ARGUMENT,
            "Component not found: " + component_id,
            "StateManager"
        );
    }

    return Result<ComponentInfo>(it->second);
}

Result<void> StateManager::register_component(const ComponentInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (info.id.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component ID cannot be empty",
            "StateManager"
        );
    }

    if (components_.find(info.id) != components_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component already registered: " + info.id,
            "StateManager"
        );
    }

    components_[info.id] = info;
    INFO("Registered component " + info.id + " of type " +
         std::to_string(static_cast<int>(info.type)));

    return Result<void>();  // Success case for void
}

Result<void> StateManager::update_metrics(
    const std::string& component_id,
    const std::unordered_map<std::string, double>& metrics) {
    
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component not found: " + component_id,
            "StateManager"
        );
    }

    // Update metrics
    it->second.metrics = metrics;
    it->second.last_update = std::chrono::system_clock::now();

    return Result<void>();  // Success case for void
}

bool StateManager::is_healthy() const {
    std::lock_guard<std::mutex> lock(mutex_);

    // System is healthy if all components are either INITIALIZED or RUNNING
    for (const auto& [id, info] : components_) {
        if (info.state != ComponentState::INITIALIZED &&
            info.state != ComponentState::RUNNING) {
            return false;
        }
    }

    return !components_.empty();
}

Result<void> StateManager::validate_transition(
    ComponentState current_state,
    ComponentState new_state) const {
    
    // Define valid transitions
    bool valid = false;
    switch (current_state) {
        case ComponentState::INITIALIZED:
            valid = (new_state == ComponentState::RUNNING ||
                    new_state == ComponentState::ERROR);
            break;

        case ComponentState::RUNNING:
            valid = (new_state == ComponentState::PAUSED ||
                    new_state == ComponentState::STOPPED ||
                    new_state == ComponentState::ERROR);
            break;

        case ComponentState::PAUSED:
            valid = (new_state == ComponentState::RUNNING ||
                    new_state == ComponentState::STOPPED ||
                    new_state == ComponentState::ERROR);
            break;

        case ComponentState::ERROR:
            valid = (new_state == ComponentState::INITIALIZED ||
                    new_state == ComponentState::STOPPED);
            break;

        case ComponentState::STOPPED:
            valid = (new_state == ComponentState::INITIALIZED);
            break;
    }

    if (!valid) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid state transition from " + 
            std::to_string(static_cast<int>(current_state)) + " to " +
            std::to_string(static_cast<int>(new_state)),
            "StateManager"
        );
    }

    return Result<void>();  // Success case for void
}

} // namespace trade_ngin