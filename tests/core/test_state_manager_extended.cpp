// Extended branch coverage for state_manager.cpp. Targets every state
// transition path, error returns, metrics, list, shutdown.

#include <gtest/gtest.h>
#include "trade_ngin/core/state_manager.hpp"

using namespace trade_ngin;

namespace {

ComponentInfo make_info(const std::string& id, ComponentState state = ComponentState::INITIALIZED) {
    return ComponentInfo{ComponentType::STRATEGY, state, id, "", std::chrono::system_clock::now(), {}};
}

}  // namespace

class StateManagerExtendedTest : public ::testing::Test {
protected:
    void SetUp() override { StateManager::reset_instance(); }
    void TearDown() override { StateManager::reset_instance(); }
};

// ===== Registration error paths =====

TEST_F(StateManagerExtendedTest, RegisterRejectsEmptyId) {
    auto info = make_info("");
    auto r = StateManager::instance().register_component(info);
    EXPECT_TRUE(r.is_error());
}

TEST_F(StateManagerExtendedTest, UnregisterReturnsErrorForUnknownComponent) {
    auto r = StateManager::instance().unregister_component("does-not-exist");
    EXPECT_TRUE(r.is_error());
}

TEST_F(StateManagerExtendedTest, UnregisterRemovesRegisteredComponent) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c1")).is_ok());
    auto r = StateManager::instance().unregister_component("c1");
    EXPECT_TRUE(r.is_ok());
    EXPECT_TRUE(StateManager::instance().get_state("c1").is_error());
}

// ===== get_state =====

TEST_F(StateManagerExtendedTest, GetStateReturnsErrorForMissing) {
    auto r = StateManager::instance().get_state("nope");
    EXPECT_TRUE(r.is_error());
}

TEST_F(StateManagerExtendedTest, GetStateReturnsRegisteredInfo) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c2")).is_ok());
    auto r = StateManager::instance().get_state("c2");
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value().id, "c2");
}

// ===== update_state transitions =====

TEST_F(StateManagerExtendedTest, UpdateStateOnUnknownComponentErrors) {
    auto r = StateManager::instance().update_state("nope", ComponentState::RUNNING);
    EXPECT_TRUE(r.is_error());
}

TEST_F(StateManagerExtendedTest, UpdateStateInitializedToErrorClearsThenSetsErrorMessage) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c3")).is_ok());
    auto r =
        StateManager::instance().update_state("c3", ComponentState::ERR_STATE, "boom");
    ASSERT_TRUE(r.is_ok());
    auto info = StateManager::instance().get_state("c3");
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().state, ComponentState::ERR_STATE);
    EXPECT_EQ(info.value().error_message, "boom");
}

TEST_F(StateManagerExtendedTest, UpdateStateClearsErrorMessageOnNonErrorTransition) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c4")).is_ok());
    ASSERT_TRUE(StateManager::instance()
                    .update_state("c4", ComponentState::ERR_STATE, "fault")
                    .is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c4", ComponentState::INITIALIZED).is_ok());
    auto info = StateManager::instance().get_state("c4");
    ASSERT_TRUE(info.is_ok());
    EXPECT_TRUE(info.value().error_message.empty());
}

TEST_F(StateManagerExtendedTest, UpdateStateValidTransitionRunningToPaused) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c5")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c5", ComponentState::RUNNING).is_ok());
    EXPECT_TRUE(StateManager::instance().update_state("c5", ComponentState::PAUSED).is_ok());
}

TEST_F(StateManagerExtendedTest, UpdateStateValidTransitionRunningToStopped) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c6")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c6", ComponentState::RUNNING).is_ok());
    EXPECT_TRUE(StateManager::instance().update_state("c6", ComponentState::STOPPED).is_ok());
}

TEST_F(StateManagerExtendedTest, UpdateStateValidTransitionPausedToRunning) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c7")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c7", ComponentState::RUNNING).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c7", ComponentState::PAUSED).is_ok());
    EXPECT_TRUE(StateManager::instance().update_state("c7", ComponentState::RUNNING).is_ok());
}

TEST_F(StateManagerExtendedTest, UpdateStateValidTransitionPausedToStopped) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c8")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c8", ComponentState::RUNNING).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c8", ComponentState::PAUSED).is_ok());
    EXPECT_TRUE(StateManager::instance().update_state("c8", ComponentState::STOPPED).is_ok());
}

TEST_F(StateManagerExtendedTest, UpdateStateValidTransitionStoppedToInitialized) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c9")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c9", ComponentState::RUNNING).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c9", ComponentState::STOPPED).is_ok());
    EXPECT_TRUE(StateManager::instance().update_state("c9", ComponentState::INITIALIZED).is_ok());
}

TEST_F(StateManagerExtendedTest, UpdateStateInvalidTransitionStoppedToRunningErrors) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c10")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c10", ComponentState::RUNNING).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c10", ComponentState::STOPPED).is_ok());
    EXPECT_TRUE(StateManager::instance()
                    .update_state("c10", ComponentState::RUNNING)
                    .is_error());
}

TEST_F(StateManagerExtendedTest, UpdateStateInvalidTransitionRunningToInitializedErrors) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("c11")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("c11", ComponentState::RUNNING).is_ok());
    EXPECT_TRUE(StateManager::instance()
                    .update_state("c11", ComponentState::INITIALIZED)
                    .is_error());
}

// ===== Health =====

TEST_F(StateManagerExtendedTest, EmptyComponentsIsNotHealthy) {
    EXPECT_FALSE(StateManager::instance().is_healthy());
}

TEST_F(StateManagerExtendedTest, ErrorStateMakesUnhealthy) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("h1")).is_ok());
    ASSERT_TRUE(StateManager::instance()
                    .update_state("h1", ComponentState::ERR_STATE, "fail")
                    .is_ok());
    EXPECT_FALSE(StateManager::instance().is_healthy());
}

TEST_F(StateManagerExtendedTest, PausedStateMakesUnhealthy) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("h2")).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("h2", ComponentState::RUNNING).is_ok());
    ASSERT_TRUE(StateManager::instance().update_state("h2", ComponentState::PAUSED).is_ok());
    EXPECT_FALSE(StateManager::instance().is_healthy());
}

// ===== Metrics =====

TEST_F(StateManagerExtendedTest, UpdateMetricsForUnknownComponentErrors) {
    auto r = StateManager::instance().update_metrics("nope", {{"a", 1.0}});
    EXPECT_TRUE(r.is_error());
}

TEST_F(StateManagerExtendedTest, UpdateMetricsStoresMetrics) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("m1")).is_ok());
    ASSERT_TRUE(
        StateManager::instance().update_metrics("m1", {{"latency_ms", 12.5}, {"qps", 99.0}}).is_ok());
    auto info = StateManager::instance().get_state("m1");
    ASSERT_TRUE(info.is_ok());
    EXPECT_DOUBLE_EQ(info.value().metrics.at("latency_ms"), 12.5);
}

// ===== get_all_components / shutdown =====

TEST_F(StateManagerExtendedTest, GetAllComponentsListsAllRegistered) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("a")).is_ok());
    ASSERT_TRUE(StateManager::instance().register_component(make_info("b")).is_ok());
    auto ids = StateManager::instance().get_all_components();
    EXPECT_EQ(ids.size(), 2u);
}

TEST_F(StateManagerExtendedTest, ShutdownClearsAllComponents) {
    ASSERT_TRUE(StateManager::instance().register_component(make_info("z")).is_ok());
    StateManager::instance().shutdown();
    EXPECT_TRUE(StateManager::instance().get_all_components().empty());
}
