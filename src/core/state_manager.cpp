//===== state_manager.cpp =====
#include "trade_ngin/core/state_manager.hpp"

namespace trade_ngin {

Result<void> StateManager::register_component(const ComponentInfo& info) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    if (info.id.empty()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component ID cannot be empty",
            "StateManager"
        );
    }

    if (components_.count(info.id)) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component already registered: " + info.id,
            "StateManager"
        );
    }
    
    components_[info.id] = info;
    cv_.notify_all();
    return Result<void>();
}

Result<void> StateManager::unregister_component(const std::string& component_id) {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component not found: " + component_id,
            "StateManager"
        );
    }

    components_.erase(it);
    cv_.notify_all();
    return Result<void>();
}

Result<ComponentInfo> StateManager::get_state(const std::string& component_id) const {
    std::unique_lock<std::recursive_mutex> lock(mutex_);

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

Result<void> StateManager::update_state(
    const std::string& component_id,
    ComponentState new_state,
    const std::string& error_message) {
    
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component not found: " + component_id,
            "StateManager"
        );
    }

    auto validation = validate_transition(it->second.state, new_state);
    if (validation.is_error()) {
        return validation;
    }

    it->second.state = new_state;
    it->second.last_update = std::chrono::system_clock::now();

    if (new_state == ComponentState::ERR_STATE) {
        it->second.error_message = error_message;
    } else {
        it->second.error_message.clear();
    }

    return Result<void>();
}

Result<void> StateManager::validate_transition(
    ComponentState current_state,
    ComponentState new_state) const {
    
    bool valid = false;
    switch (current_state) {
        case ComponentState::INITIALIZED:
            valid = (new_state == ComponentState::RUNNING ||
                    new_state == ComponentState::ERR_STATE);
            break;
        case ComponentState::RUNNING:
            valid = (new_state == ComponentState::PAUSED ||
                    new_state == ComponentState::STOPPED ||
                    new_state == ComponentState::ERR_STATE);
            break;
        case ComponentState::PAUSED:
            valid = (new_state == ComponentState::RUNNING ||
                    new_state == ComponentState::STOPPED ||
                    new_state == ComponentState::ERR_STATE);
            break;
        case ComponentState::ERR_STATE:
            valid = (new_state == ComponentState::INITIALIZED ||
                    new_state == ComponentState::STOPPED);
            break;
        case ComponentState::STOPPED:
            valid = new_state == ComponentState::INITIALIZED;
            break;
    }

    if (!valid) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid state transition",
            "StateManager"
        );
    }

    return Result<void>();
}

bool StateManager::is_healthy() const {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (components_.empty()) return false;

    for (const auto& [_, info] : components_) {
        if (info.state != ComponentState::INITIALIZED &&
            info.state != ComponentState::RUNNING) {
            return false;
        }
    }
    return true;
}

Result<void> StateManager::update_metrics(
    const std::string& component_id,
    const std::unordered_map<std::string, double>& metrics) {
    
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    auto it = components_.find(component_id);
    if (it == components_.end()) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Component not found: " + component_id,
            "StateManager"
        );
    }

    it->second.metrics = metrics;
    it->second.last_update = std::chrono::system_clock::now();
    return Result<void>();
}

std::vector<std::string> StateManager::get_all_components() const {
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(components_.size());
    for (const auto& [id, _] : components_) {
        ids.push_back(id);
    }
    return ids;
}
}  // namespace trade_ngin