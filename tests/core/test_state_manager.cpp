// tests/core/test_state_manager.cpp
#include <gtest/gtest.h>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/error.hpp"

using namespace trade_ngin;

class StateManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset the state manager before each test
        auto& manager = StateManager::instance();
        // Clear any existing components by re-registering them
        // This is a test-only operation that you might want to add to StateManager
    }
};

TEST_F(StateManagerTest, RegisterComponent) {
    auto& manager = StateManager::instance();
    
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "TEST_COMPONENT",
        "",
        std::chrono::system_clock::now(),
        {{"metric1", 1.0}, {"metric2", 2.0}}
    };

    auto result = manager.register_component(info);
    EXPECT_TRUE(result.is_ok());

    // Try to register same component again - should fail
    auto duplicate_result = manager.register_component(info);
    EXPECT_TRUE(duplicate_result.is_error());
    EXPECT_EQ(duplicate_result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, UpdateState) {
    auto& manager = StateManager::instance();
    
    // First register a component
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "TEST_COMPONENT",
        "",
        std::chrono::system_clock::now(),
        {}
    };
    
    ASSERT_TRUE(manager.register_component(info).is_ok());

    // Test valid state transition
    auto result = manager.update_state("TEST_COMPONENT", ComponentState::RUNNING);
    EXPECT_TRUE(result.is_ok());

    // Verify state was updated
    auto state_result = manager.get_state("TEST_COMPONENT");
    ASSERT_TRUE(state_result.is_ok());
    EXPECT_EQ(state_result.value().state, ComponentState::RUNNING);

    // Test invalid state transition
    auto invalid_result = manager.update_state("TEST_COMPONENT", ComponentState::INITIALIZED);
    EXPECT_TRUE(invalid_result.is_error());
    EXPECT_EQ(invalid_result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, UpdateMetrics) {
    auto& manager = StateManager::instance();
    
    // Register component
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "TEST_COMPONENT",
        "",
        std::chrono::system_clock::now(),
        {{"initial_metric", 1.0}}
    };
    
    ASSERT_TRUE(manager.register_component(info).is_ok());

    // Update metrics
    std::unordered_map<std::string, double> new_metrics{
        {"metric1", 10.0},
        {"metric2", 20.0}
    };

    auto result = manager.update_metrics("TEST_COMPONENT", new_metrics);
    EXPECT_TRUE(result.is_ok());

    // Verify metrics were updated
    auto state_result = manager.get_state("TEST_COMPONENT");
    ASSERT_TRUE(state_result.is_ok());
    EXPECT_EQ(state_result.value().metrics, new_metrics);
}

TEST_F(StateManagerTest, SystemHealth) {
    auto& manager = StateManager::instance();
    
    // System should not be healthy with no components
    EXPECT_FALSE(manager.is_healthy());

    // Add a healthy component
    ComponentInfo info1{
        ComponentType::STRATEGY,
        ComponentState::RUNNING,
        "COMPONENT1",
        "",
        std::chrono::system_clock::now(),
        {}
    };
    ASSERT_TRUE(manager.register_component(info1).is_ok());
    EXPECT_TRUE(manager.is_healthy());

    // Add an unhealthy component
    ComponentInfo info2{
        ComponentType::MARKET_DATA,
        ComponentState::ERROR,
        "COMPONENT2",
        "Error state",
        std::chrono::system_clock::now(),
        {}
    };
    ASSERT_TRUE(manager.register_component(info2).is_ok());
    EXPECT_FALSE(manager.is_healthy());
}

TEST_F(StateManagerTest, NonExistentComponent) {
    auto& manager = StateManager::instance();
    
    // Try to update non-existent component
    auto update_result = manager.update_state("NONEXISTENT", ComponentState::RUNNING);
    EXPECT_TRUE(update_result.is_error());
    EXPECT_EQ(update_result.error()->code(), ErrorCode::INVALID_ARGUMENT);

    // Try to get state of non-existent component
    auto state_result = manager.get_state("NONEXISTENT");
    EXPECT_TRUE(state_result.is_error());
    EXPECT_EQ(state_result.error()->code(), ErrorCode::INVALID_ARGUMENT);

    // Try to update metrics of non-existent component
    auto metrics_result = manager.update_metrics("NONEXISTENT", {{"metric", 1.0}});
    EXPECT_TRUE(metrics_result.is_error());
    EXPECT_EQ(metrics_result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, StateTransitions) {
    auto& manager = StateManager::instance();
    
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "TEST_COMPONENT",
        "",
        std::chrono::system_clock::now(),
        {}
    };
    
    ASSERT_TRUE(manager.register_component(info).is_ok());

    // Test all valid transitions
    std::vector<std::pair<ComponentState, ComponentState>> valid_transitions = {
        {ComponentState::INITIALIZED, ComponentState::RUNNING},
        {ComponentState::RUNNING, ComponentState::PAUSED},
        {ComponentState::PAUSED, ComponentState::RUNNING},
        {ComponentState::RUNNING, ComponentState::STOPPED},
        {ComponentState::PAUSED, ComponentState::STOPPED}
    };

    for (const auto& [from_state, to_state] : valid_transitions) {
        // First set the initial state
        if (from_state != ComponentState::INITIALIZED) {
            ASSERT_TRUE(manager.update_state("TEST_COMPONENT", from_state).is_ok());
        }
        
        // Then try the transition
        auto result = manager.update_state("TEST_COMPONENT", to_state);
        EXPECT_TRUE(result.is_ok()) 
            << "Failed to transition from " 
            << static_cast<int>(from_state) 
            << " to " 
            << static_cast<int>(to_state);
    }

    // Test some invalid transitions
    std::vector<std::pair<ComponentState, ComponentState>> invalid_transitions = {
        {ComponentState::STOPPED, ComponentState::RUNNING},
        {ComponentState::RUNNING, ComponentState::INITIALIZED},
        {ComponentState::PAUSED, ComponentState::INITIALIZED},
        {ComponentState::STOPPED, ComponentState::PAUSED}
    };

    for (const auto& [from_state, to_state] : invalid_transitions) {
        // First set the initial state (if possible)
        if (from_state != ComponentState::STOPPED) {
            ASSERT_TRUE(manager.update_state("TEST_COMPONENT", from_state).is_ok());
        }
        
        // Then try the invalid transition
        auto result = manager.update_state("TEST_COMPONENT", to_state);
        EXPECT_TRUE(result.is_error())
            << "Unexpectedly succeeded in transitioning from "
            << static_cast<int>(from_state)
            << " to "
            << static_cast<int>(to_state);
        EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    }
}