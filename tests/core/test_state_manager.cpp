// tests/core/test_state_manager.cpp
#include <gtest/gtest.h>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include <filesystem>

using namespace trade_ngin;

class StateManagerTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Create test directory
        if (std::filesystem::exists(test_log_dir)) {
            std::filesystem::remove_all(test_log_dir);
        }
        std::filesystem::create_directories(test_log_dir);

        // Initialize logger once for all tests
        LoggerConfig logger_config;
        logger_config.min_level = LogLevel::INFO;
        logger_config.destination = LogDestination::FILE;
        logger_config.log_directory = test_log_dir;
        logger_config.filename_prefix = "test_log";
        
        auto& logger = Logger::instance();
        logger.initialize(logger_config);
    }

    static void TearDownTestSuite() {
        // Clean up at the end of all tests
        try {
            // Close any open file handles
            auto& logger = Logger::instance();
            // Ideally, add a shutdown() method to Logger to properly close files
            
            // Give a small delay to ensure files are closed
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            if (std::filesystem::exists(test_log_dir)) {
                std::filesystem::remove_all(test_log_dir);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in TearDownTestSuite: " << e.what() << std::endl;
        }
    }

    void SetUp() override {
        // Clear any existing components before each test
        components_ = std::make_unique<StateManager>();
    }

    void TearDown() override {
        // Nothing needed here as we're using a fresh StateManager for each test
    }

    // Helper to get a fresh StateManager instance for each test
    StateManager& get_manager() {
        return *components_;
    }

protected:
    static inline std::string test_log_dir = "test_logs";
    std::unique_ptr<StateManager> components_;
};

TEST_F(StateManagerTest, RegisterComponent) {
    auto& manager = get_manager();
    
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "TEST_COMPONENT",
        "",
        std::chrono::system_clock::now(),
        {{"metric1", 1.0}, {"metric2", 2.0}}
    };

    auto result = manager.register_component(info);
    EXPECT_TRUE(result.is_ok()) << "Failed to register component: " 
        << (result.is_error() ? result.error()->what() : "Unknown error");

    // Try to register same component again - should fail
    auto duplicate_result = manager.register_component(info);
    EXPECT_TRUE(duplicate_result.is_error());
    EXPECT_EQ(duplicate_result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(StateManagerTest, UpdateState) {
    auto& manager = get_manager();
    
    // First register a component
    ComponentInfo info{
        ComponentType::STRATEGY,
        ComponentState::INITIALIZED,
        "TEST_COMPONENT",
        "",
        std::chrono::system_clock::now(),
        {}
    };
    
    ASSERT_TRUE(manager.register_component(info).is_ok()) 
        << "Failed to register component for UpdateState test";

    // Test valid state transition
    auto result = manager.update_state("TEST_COMPONENT", ComponentState::RUNNING);
    EXPECT_TRUE(result.is_ok()) << "Failed to update state: " 
        << (result.is_error() ? result.error()->what() : "Unknown error");

    // Verify state was updated
    auto state_result = manager.get_state("TEST_COMPONENT");
    ASSERT_TRUE(state_result.is_ok()) << "Failed to get state: "
        << (state_result.is_error() ? state_result.error()->what() : "Unknown error");
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
    
    ASSERT_TRUE(manager.register_component(info).is_ok())
        << "Failed to register component for UpdateMetrics test";

    // Update metrics
    std::unordered_map<std::string, double> new_metrics{
        {"metric1", 10.0},
        {"metric2", 20.0}
    };

    auto result = manager.update_metrics("TEST_COMPONENT", new_metrics);
    EXPECT_TRUE(result.is_ok()) << "Failed to update metrics: "
        << (result.is_error() ? result.error()->what() : "Unknown error");

    // Verify metrics were updated
    auto state_result = manager.get_state("TEST_COMPONENT");
    ASSERT_TRUE(state_result.is_ok());
    EXPECT_EQ(state_result.value().metrics, new_metrics);
}

TEST_F(StateManagerTest, SystemHealth) {
    auto& manager = StateManager::instance();
    
    // System should not be healthy with no components
    EXPECT_FALSE(manager.is_healthy()) << "System should not be healthy with no components";

    // Add a healthy component
    ComponentInfo info1{
        ComponentType::STRATEGY,
        ComponentState::RUNNING,
        "COMPONENT1",
        "",
        std::chrono::system_clock::now(),
        {}
    };
    ASSERT_TRUE(manager.register_component(info1).is_ok())
        << "Failed to register healthy component";
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
    ASSERT_TRUE(manager.register_component(info2).is_ok())
        << "Failed to register unhealthy component";
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
    
    ASSERT_TRUE(manager.register_component(info).is_ok())
        << "Failed to register component for StateTransitions test";

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
            ASSERT_TRUE(manager.update_state("TEST_COMPONENT", from_state).is_ok())
                << "Failed to set initial state in transition test";
        }
        
        // Then try the transition
        auto result = manager.update_state("TEST_COMPONENT", to_state);
        EXPECT_TRUE(result.is_ok()) 
            << "Failed to transition from " 
            << static_cast<int>(from_state) 
            << " to " 
            << static_cast<int>(to_state)
            << ": " << (result.is_error() ? result.error()->what() : "Unknown error");
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
            ASSERT_TRUE(manager.update_state("TEST_COMPONENT", from_state).is_ok())
                << "Failed to set initial state for invalid transition test";
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