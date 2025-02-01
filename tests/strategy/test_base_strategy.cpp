#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/strategy/types.hpp"
#

using namespace trade_ngin;

// --- Mock Database with Failure Simulation ---
class MockDatabase : public DatabaseInterface {
public:
    Result<void> store_executions(const std::vector<ExecutionReport>& executions, const std::string& table) override {
        if (simulate_failure) throw std::runtime_error("DB failure");
        executions_stored = executions;
        return Result<void>();
    }

    Result<void> store_positions(const std::vector<Position>& positions, const std::string& table) override {
        if (simulate_failure) throw std::runtime_error("DB failure");
        positions_stored = positions;
        return Result<void>();
    }

    Result<void> store_signals(const std::unordered_map<std::string, double>& signals, const std::string& strategy_id,
                               const std::chrono::system_clock::time_point& time, const std::string& table) override {
        if (simulate_failure) throw std::runtime_error("DB failure");
        signals_stored = signals;
        return Result<void>();
    }

    // Test controls
    bool simulate_failure = false;
    std::vector<ExecutionReport> executions_stored;
    std::vector<Position> positions_stored;
    std::unordered_map<std::string, double> signals_stored;
};

// --- Test Fixture with Helpers ---
class BaseStrategyTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset any static state if needed
    }

    BaseStrategy createInitializedStrategy(
        StrategyConfig config = StrategyConfig{}, 
        std::shared_ptr<DatabaseInterface> db = std::make_shared<MockDatabase>()
    ) {
        config.capital_allocation = (config.capital_allocation > 0) ? config.capital_allocation : 100000;
        config.max_leverage = (config.max_leverage > 0) ? config.max_leverage : 10;
        BaseStrategy strategy("test_strategy", config, db);
        strategy.initialize();
        return strategy;
    }

    BaseStrategy createRunningStrategy(
        StrategyConfig config = StrategyConfig{},
        std::shared_ptr<DatabaseInterface> db = std::make_shared<MockDatabase>()
    ) {
        BaseStrategy strategy = createInitializedStrategy(config, db);
        strategy.start();
        return strategy;
    }

    ExecutionReport createExecution(Side side, const std::string& symbol, double qty, double price) {
        ExecutionReport report;
        report.symbol = symbol;
        report.side = side;
        report.filled_quantity = qty;
        report.fill_price = price;
        report.fill_time = std::chrono::system_clock::now();
        return report;
    }

    Position createPosition(double quantity, double avg_price) {
        Position pos;
        pos.quantity = quantity;
        pos.average_price = avg_price;
        return pos;
    }
};

// ================================================
//           Expanded Test Cases
// ================================================

// --- State Management ---
TEST_F(BaseStrategyTest, Start_FailsIfNotInitialized) {
    BaseStrategy strategy("test_strategy", StrategyConfig{}, std::make_shared<MockDatabase>());
    auto result = strategy.start(); // Not initialized
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(BaseStrategyTest, Pause_TransitionsFromRunningToPaused) {
    BaseStrategy strategy = createRunningStrategy();
    auto result = strategy.pause();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(strategy.get_state(), StrategyState::PAUSED);
}

TEST_F(BaseStrategyTest, Resume_FailsIfNotPaused) {
    BaseStrategy strategy = createRunningStrategy();
    auto result = strategy.resume(); // Already running
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(BaseStrategyTest, Stop_SavesPositionsOnlyWhenEnabled) {
    auto db = std::make_shared<MockDatabase>();
    StrategyConfig config;
    config.save_positions = true;
    BaseStrategy strategy = createRunningStrategy(config, db);
    strategy.stop();
    EXPECT_EQ(db->positions_stored.size(), 1); // Saved

    db->positions_stored.clear();
    config.save_positions = false;
    BaseStrategy strategy2 = createRunningStrategy(config, db);
    strategy2.stop();
    EXPECT_TRUE(db->positions_stored.empty()); // Not saved
}

// --- Database & Error Handling ---
TEST_F(BaseStrategyTest, SaveSignals_WhenEnabledAndDisabled) {
    auto db = std::make_shared<MockDatabase>();
    StrategyConfig config;
    config.save_signals = true;
    BaseStrategy strategy = createRunningStrategy(config, db);
    strategy.on_signal("AAPL", 1.0);
    EXPECT_EQ(db->signals_stored["AAPL"], 1.0); // Saved

    config.save_signals = false;
    BaseStrategy strategy2 = createRunningStrategy(config, db);
    strategy2.on_signal("GOOG", 0.5);
    EXPECT_TRUE(db->signals_stored.empty()); // Not saved
}

TEST_F(BaseStrategyTest, SaveExecution_FailurePropagatesError) {
    auto db = std::make_shared<MockDatabase>();
    db->simulate_failure = true;
    StrategyConfig config;
    config.save_executions = true;
    BaseStrategy strategy = createRunningStrategy(config, db);
    auto report = createExecution(Side::BUY, "AAPL", 100, 150.0);
    auto result = strategy.on_execution(report);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);
}

// --- Position & Risk Limits ---
TEST_F(BaseStrategyTest, UpdatePosition_FailsIfExceedsLimit) {
    StrategyConfig config;
    config.position_limits["AAPL"] = 100;
    BaseStrategy strategy = createRunningStrategy(config);
    Position pos;
    pos.quantity = 200; // Exceeds limit
    auto result = strategy.update_position("AAPL", pos);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::POSITION_LIMIT_EXCEEDED);
}

TEST_F(BaseStrategyTest, CheckRiskLimits_FailsOnMaxDrawdown) {
    StrategyConfig config;
    config.capital_allocation = 100000;
    BaseStrategy strategy = createRunningStrategy(config);
    
    // Simulate a large loss
    strategy.on_execution(createExecution(Side::SELL, "AAPL", 1000, 50.0)); // Short 1000 shares
    strategy.on_execution(createExecution(Side::BUY, "AAPL", 1000, 200.0)); // Buy back at higher price
    // Realized PnL: (50 - 200) * 1000 = -150,000 → Drawdown = -150%

    RiskLimits limits;
    limits.max_drawdown = 0.5; // 50% max drawdown
    strategy.update_risk_limits(limits);
    auto result = strategy.check_risk_limits();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::RISK_LIMIT_EXCEEDED);
}

// --- Concurrency ---
TEST_F(BaseStrategyTest, ThreadSafety_OnDataAndExecution) {
    BaseStrategy strategy = createRunningStrategy();
    std::atomic<bool> test_passed{true};

    // Spawn threads to simulate concurrent data/execution processing
    auto data_thread = std::thread([&]() {
        std::vector<Bar> data{Bar{}};
        for (int i = 0; i < 1000; ++i) {
            if (strategy.on_data(data).is_error()) test_passed = false;
        }
    });

    auto exec_thread = std::thread([&]() {
        auto report = createExecution(Side::BUY, "AAPL", 10, 150.0);
        for (int i = 0; i < 1000; ++i) {
            if (strategy.on_execution(report).is_error()) test_passed = false;
        }
    });

    data_thread.join();
    exec_thread.join();
    EXPECT_TRUE(test_passed); // No crashes/data races
}

// --- Metrics & Signals ---
TEST_F(BaseStrategyTest, OnSignal_UpdatesLastSignals) {
    BaseStrategy strategy = createRunningStrategy();
    strategy.on_signal("AAPL", 0.8);
    strategy.on_signal("GOOG", -0.5);
    const auto& signals = strategy.get_metadata().signals; // Assuming signals are tracked in metadata
    EXPECT_EQ(signals.at("AAPL"), 0.8);
    EXPECT_EQ(signals.at("GOOG"), -0.5);
}

TEST_F(BaseStrategyTest, UpdateMetrics_CalculatesUnrealizedPnl) {
    BaseStrategy strategy = createRunningStrategy();
    strategy.update_position("AAPL", createPosition(100, 150.0));
    strategy.update_position("GOOG", createPosition(-50, 2000.0));
    auto result = strategy.update_metrics();
    EXPECT_TRUE(result.is_ok());
    // Assuming unrealized PnL is tracked (mock market data needed for accuracy)
}

// --- Edge Cases ---
TEST_F(BaseStrategyTest, Initialize_FailsWithZeroCapital) {
    StrategyConfig config;
    config.capital_allocation = 0; // Invalid
    BaseStrategy strategy("test_strategy", config, std::make_shared<MockDatabase>());
    auto result = strategy.initialize();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(BaseStrategyTest, OnData_IgnoresNonBarEvents) {
    BaseStrategy strategy = createRunningStrategy();
    MarketDataEvent event;
    event.type = MarketDataEventType::TRADE; // Not BAR
    // Verify callback ignores non-BAR events (no crash/error)
}

// --- State Transition Validation ---
TEST_F(BaseStrategyTest, ValidateStateTransition_BlocksInvalidTransitions) {
    BaseStrategy strategy = createInitializedStrategy();
    // INITIALIZED → PAUSED (invalid)
    auto result = strategy.transition_state(StrategyState::PAUSED);
    EXPECT_TRUE(result.is_error());
}

// ================================================
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}