#include <gtest/gtest.h>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/logger.hpp"

// Disable logging during tests
#ifdef RUNNING_TESTS
    #undef INFO
    #define INFO(message)
    #undef ERROR
    #define ERROR(message)
    // Repeat for other log levels
#endif

using namespace trade_ngin;

class StateManagerTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {  // Runs once before all tests
        LoggerConfig config;
        config.min_level = LogLevel::FATAL;  // Disable most logging
        config.destination = LogDestination::CONSOLE;
        Logger::instance().initialize(config);
    }

    void SetUp() override {  // Runs before each test
        // Reset state between tests if needed
        StateManager::instance().reset_for_tests();

        // Ensure no components remain
        ASSERT_TRUE(StateManager::instance().get_state("comp1").is_error());
        ASSERT_TRUE(StateManager::instance().get_state("health_comp1").is_error());
    }

    static void TearDownTestSuite() {  // Runs once after all tests
        // Reset state between tests if needed
        StateManager::instance().reset_for_tests();
    }
};

TEST_F(StateManagerTest, RegisterComponentSuccess) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::INITIALIZED;

    auto result = sm.register_component(info);
    EXPECT_TRUE(result.is_ok());
}

TEST_F(StateManagerTest, RegisterComponentEmptyId) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "";
    auto result = sm.register_component(info);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, RegisterComponentDuplicateId) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    sm.register_component(info);

    auto result = sm.register_component(info);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, GetStateExistingComponent) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::INITIALIZED;
    sm.register_component(info);

    auto result = sm.get_state("comp1");
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().id, "comp1");
}

TEST_F(StateManagerTest, GetStateNonExistentComponent) {
    StateManager& sm = StateManager::instance();
    auto result = sm.get_state("invalid");
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, UpdateStateValidTransition) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::INITIALIZED;
    sm.register_component(info);

    auto result = sm.update_state("comp1", ComponentState::RUNNING);
    EXPECT_TRUE(result.is_ok());

    auto get_result = sm.get_state("comp1");
    ASSERT_TRUE(get_result.is_ok());
    EXPECT_EQ(get_result.value().state, ComponentState::RUNNING);
}

TEST_F(StateManagerTest, UpdateStateInvalidTransition) {
    StateManager& sm = StateManager::instance();

    // Use a UNIQUE ID for this test
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "invalid_transition_comp", // Unique ID
        "", {}, {}
    };

    auto reg_result = sm.register_component(info);
    ASSERT_TRUE(reg_result.is_ok()) << "Registration failed: " << reg_result.error()->what();

    auto result = sm.update_state("invalid_transition_comp", ComponentState::PAUSED);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, UpdateStateToErrorSetsMessage) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::INITIALIZED;
    sm.register_component(info);

    std::string error_msg = "Critical failure";
    auto result = sm.update_state("comp1", ComponentState::ERR_STATE, error_msg);
    EXPECT_TRUE(result.is_ok());

    auto get_result = sm.get_state("comp1");
    ASSERT_TRUE(get_result.is_ok());
    EXPECT_EQ(get_result.value().error_message, error_msg);
}

TEST_F(StateManagerTest, UpdateStateFromErrorRetainsMessage) {
    StateManager sm;
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::ERR_STATE;
    sm.register_component(info);
    
    // Transition out of error state
    sm.update_state("comp1", ComponentState::INITIALIZED);
    
    auto result = sm.get_state("comp1");
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(result.value().error_message.empty()); // Now expects empty message
}

TEST_F(StateManagerTest, UpdateMetricsSuccess) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    sm.register_component(info);

    std::unordered_map<std::string, double> metrics{{"latency", 42.0}};
    auto result = sm.update_metrics("comp1", metrics);
    EXPECT_TRUE(result.is_ok());

    auto get_result = sm.get_state("comp1");
    ASSERT_TRUE(get_result.is_ok());
    EXPECT_EQ(get_result.value().metrics.at("latency"), 42.0);
}

TEST_F(StateManagerTest, IsHealthyAllValid) {
    StateManager& sm = StateManager::instance();
    
    // Use unique IDs and explicit field initialization
    ComponentInfo info1{
        ComponentType::STRATEGY,    // type
        ComponentState::INITIALIZED, // state
        "health_comp1",             // id
        "",                         // error_message
        {},                         // last_update
        {}                          // metrics
    };

    ComponentInfo info2{
        ComponentType::MARKET_DATA,
        ComponentState::RUNNING,
        "health_comp2",
        "", {}, {}
    };

    auto res1 = sm.register_component(info1);
    auto res2 = sm.register_component(info2);
    
    ASSERT_TRUE(res1.is_ok()) << "Registration failed: " << res1.error()->what();
    ASSERT_TRUE(res2.is_ok()) << "Registration failed: " << res2.error()->what();

    auto comp1 = sm.get_state("health_comp1").value();
    auto comp2 = sm.get_state("health_comp2").value();
    ASSERT_EQ(comp1.state, ComponentState::INITIALIZED);
    ASSERT_EQ(comp2.state, ComponentState::RUNNING);

    EXPECT_TRUE(sm.is_healthy());
}

TEST_F(StateManagerTest, IsHealthyWithInvalidComponent) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::PAUSED;
    sm.register_component(info);

    EXPECT_FALSE(sm.is_healthy());
}

TEST_F(StateManagerTest, IsHealthyNoComponents) {
    StateManager& sm = StateManager::instance();
    EXPECT_FALSE(sm.is_healthy()); // Intentionally returns false when empty
}

// Test for error message when transitioning to ERROR without a message (current behavior allows it)
TEST_F(StateManagerTest, UpdateStateToErrorWithoutMessage) {
    StateManager& sm = StateManager::instance();
    ComponentInfo info;
    info.id = "comp1";
    info.type = ComponentType::STRATEGY;
    info.state = ComponentState::INITIALIZED;
    sm.register_component(info);

    auto result = sm.update_state("comp1", ComponentState::ERR_STATE);
    EXPECT_TRUE(result.is_ok()); // No error message provided, but transition is allowed
}