//===== test_state_manager.cpp =====
#ifndef TESTING
#define TESTING
#endif

#include <gtest/gtest.h>
#include "trade_ngin/core/state_manager.hpp"

using namespace trade_ngin;

class StateManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        StateManager::reset_instance();
    }

    void TearDown() override {
        StateManager::reset_instance();
    }
};

TEST_F(StateManagerTest, RegisterComponentSuccess) {
    ComponentInfo info{ComponentType::STRATEGY,
                       ComponentState::INITIALIZED,
                       "test_component",
                       "",
                       std::chrono::system_clock::now(),
                       {}};

    auto result = StateManager::instance().register_component(info);
    EXPECT_TRUE(result.is_ok());
}

TEST_F(StateManagerTest, RegisterDuplicateComponent) {
    ComponentInfo info{ComponentType::STRATEGY,
                       ComponentState::INITIALIZED,
                       "test_component",
                       "",
                       std::chrono::system_clock::now(),
                       {}};

    ASSERT_TRUE(StateManager::instance().register_component(info).is_ok());
    auto result = StateManager::instance().register_component(info);
    EXPECT_TRUE(result.is_error());
}

TEST_F(StateManagerTest, StateTransitions) {
    ComponentInfo info{ComponentType::STRATEGY,
                       ComponentState::INITIALIZED,
                       "test_component",
                       "",
                       std::chrono::system_clock::now(),
                       {}};

    ASSERT_TRUE(StateManager::instance().register_component(info).is_ok());

    // Valid transition
    auto result1 = StateManager::instance().update_state("test_component", ComponentState::RUNNING);
    EXPECT_TRUE(result1.is_ok());

    // Invalid transition
    auto result2 =
        StateManager::instance().update_state("test_component", ComponentState::INITIALIZED);
    EXPECT_TRUE(result2.is_error());
}

TEST_F(StateManagerTest, ComponentHealth) {
    ComponentInfo info1{ComponentType::STRATEGY,
                        ComponentState::INITIALIZED,
                        "component1",
                        "",
                        std::chrono::system_clock::now(),
                        {}};

    ComponentInfo info2{ComponentType::MARKET_DATA,
                        ComponentState::RUNNING,
                        "component2",
                        "",
                        std::chrono::system_clock::now(),
                        {}};

    ASSERT_TRUE(StateManager::instance().register_component(info1).is_ok());
    ASSERT_TRUE(StateManager::instance().register_component(info2).is_ok());
    EXPECT_TRUE(StateManager::instance().is_healthy());
}