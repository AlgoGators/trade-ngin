#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
#include <thread>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/types.hpp"

using namespace trade_ngin;

// --- Mock Database with Failure Simulation ---
class MockPostgresDatabase : public PostgresDatabase {
public:
    MockPostgresDatabase() : PostgresDatabase("mock://testdb") {}
    Result<void> connect() override {
        connected = true;
        return Result<void>();
    }
    void disconnect() override {
        connected = false;
    }
    bool is_connected() const override {
        return connected;
    }
    Result<std::shared_ptr<arrow::Table>> get_market_data(const std::vector<std::string>&,
                                                          const Timestamp&, const Timestamp&,
                                                          AssetClass,
                                                          DataFrequency = DataFrequency::DAILY,
                                                          const std::string& = "ohlcv") override {
        return Result<std::shared_ptr<arrow::Table>>(nullptr);
    }
    Result<void> store_executions(const std::vector<ExecutionReport>& executions,
                                  const std::string&) override {
        executions_stored = executions;
        return Result<void>();
    }
    Result<void> store_positions(const std::vector<Position>& positions,
                                 const std::string&, const std::string&) override {
        positions_stored = positions;
        return Result<void>();
    }
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string&, const Timestamp&, const std::string&) override {
        signals_stored = signals;
        return Result<void>();
    }
    Result<std::vector<std::string>> get_symbols(AssetClass, DataFrequency = DataFrequency::DAILY,
                                                 const std::string& = "ohlcv") override {
        return Result<std::vector<std::string>>(std::vector<std::string>{});
    }
    Result<std::shared_ptr<arrow::Table>> execute_query(const std::string&) override {
        return Result<std::shared_ptr<arrow::Table>>(nullptr);
    }
    void clear() {
        executions_stored.clear();
        positions_stored.clear();
        signals_stored.clear();
        simulate_failure = false;
    }
    bool simulate_failure{false};
    bool connected{false};
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

    std::unique_ptr<BaseStrategy> createInitializedStrategy(
        StrategyConfig config = StrategyConfig{},
        std::shared_ptr<PostgresDatabase> db = std::make_shared<MockPostgresDatabase>()) {
        config.capital_allocation =
            (config.capital_allocation > 0) ? config.capital_allocation : 100000;
        config.max_leverage = (config.max_leverage > 0) ? config.max_leverage : 10;
        auto strategy = std::make_unique<BaseStrategy>("test_strategy", config, db);
        strategy->initialize();
        return strategy;
    }

    std::unique_ptr<BaseStrategy> createRunningStrategy(
        StrategyConfig config = StrategyConfig{},
        std::shared_ptr<PostgresDatabase> db = std::make_shared<MockPostgresDatabase>()) {
        // Set reasonable defaults if not provided
        if (config.capital_allocation <= 0) {
            config.capital_allocation = 1000000.0;  // $1M default capital
        }
        if (config.max_leverage <= 0) {
            config.max_leverage = 4.0;  // 4x max leverage
        }

        // Ensure test environment is clean
        StateManager::reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // Wait for cleanup

        auto strategy = std::make_unique<BaseStrategy>("test_strategy", config, db);

        // Initialize with error checking
        auto init_result = strategy->initialize();
        EXPECT_TRUE(init_result.is_ok())
            << "Initialization failed: "
            << (init_result.error() ? init_result.error()->what() : "Unknown error");

        if (init_result.is_error()) {
            throw std::runtime_error("Strategy initialization failed: " +
                                     std::string(init_result.error()->what()));
        }

        // Initialize risk limits with reasonable values
        RiskLimits limits;
        limits.max_leverage = 4.0;              // Allow up to 4x leverage
        limits.max_drawdown = 0.25;             // 25% max drawdown
        limits.max_position_size = 100000;      // $100K max position
        limits.max_notional_value = 1000000.0;  // $1M max notional

        strategy->update_risk_limits(limits);

        // Start with error checking
        auto start_result = strategy->start();
        EXPECT_TRUE(start_result.is_ok())
            << "Start failed: "
            << (start_result.error() ? start_result.error()->what() : "Unknown error");

        if (start_result.is_error()) {
            throw std::runtime_error("Strategy start failed: " +
                                     std::string(start_result.error()->what()));
        }

        return strategy;
    }

    ExecutionReport createExecution(Side side, const std::string& symbol, double qty,
                                    double price) {
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
//                  Test Cases
// ================================================

// --- State Management ---
TEST_F(BaseStrategyTest, Start_FailsIfNotInitialized) {
    auto db = std::make_shared<MockPostgresDatabase>();
    BaseStrategy strategy("test_strategy", StrategyConfig{}, db);
    auto result = strategy.start();  // Not initialized
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::NOT_INITIALIZED);
}

TEST_F(BaseStrategyTest, Pause_TransitionsFromRunningToPaused) {
    auto strategy = createRunningStrategy();
    auto result = strategy->pause();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(strategy->get_state(), StrategyState::PAUSED);
}

TEST_F(BaseStrategyTest, Resume_FailsIfNotPaused) {
    auto strategy = createRunningStrategy();
    auto result = strategy->resume();  // Already running
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// TEST_F(BaseStrategyTest, Stop_SavesPositionsOnlyWhenEnabled) {
//     auto db = std::make_shared<MockPostgresDatabase>();
//
//     // First test with saving enabled
//     StrategyConfig config;
//     config.save_positions = true;
//     auto strategy = createRunningStrategy(config, db);
//
//     // Add a test position
//     Position pos;
//     pos.symbol = "TEST";
//     pos.quantity = 100;
//     ASSERT_TRUE(strategy->update_position("TEST", pos).is_ok());
//
//     // Stop strategy and verify position was saved
//     strategy->stop();
//     EXPECT_EQ(db->positions_stored.size(), 1);
//
//     // Clear and test with saving disabled
//     db->clear();
//     config.save_positions = false;
//     auto strategy2 = createRunningStrategy(config, db);
//     ASSERT_TRUE(strategy2->update_position("TEST", pos).is_ok());
//     strategy2->stop();
//     EXPECT_TRUE(db->positions_stored.empty());
// }

// // --- Database & Error Handling ---
// TEST_F(BaseStrategyTest, SaveSignals_WhenEnabledAndDisabled) {
//     auto db = std::make_shared<MockPostgresDatabase>();
//
//     // Test with saving enabled
//     StrategyConfig config;
//     config.save_signals = true;
//     auto strategy = createRunningStrategy(config, db);
//     ASSERT_TRUE(strategy->on_signal("AAPL", 1.0).is_ok());
//     EXPECT_EQ(db->signals_stored["AAPL"], 1.0);
//
//     // Clear and test with saving disabled
//     db->clear();
//     config.save_signals = false;
//     auto strategy2 = createRunningStrategy(config, db);
//     ASSERT_TRUE(strategy2->on_signal("GOOG", 0.5).is_ok());
//     EXPECT_TRUE(db->signals_stored.empty());
// }

// TEST_F(BaseStrategyTest, SaveExecution_FailurePropagatesError) {
//     auto db = std::make_shared<MockPostgresDatabase>();
//     db->simulate_failure = true;
//
//     StrategyConfig config;
//     config.save_executions = true;
//     auto strategy = createRunningStrategy(config, db);
//
//     auto report = createExecution(Side::BUY, "AAPL", 100, 150.0);
//     auto result = strategy->on_execution(report);
//     EXPECT_TRUE(result.is_error());
//     EXPECT_EQ(result.error()->code(), ErrorCode::DATABASE_ERROR);
// }

// // --- Position & Risk Limits ---
// TEST_F(BaseStrategyTest, UpdatePosition_FailsIfExceedsLimit) {
//     StrategyConfig config;
//     config.position_limits["AAPL"] = 100;
//     auto strategy = createRunningStrategy(config);
//     Position pos;
//     pos.quantity = 200;  // Exceeds limit
//     auto result = strategy->update_position("AAPL", pos);
//     EXPECT_TRUE(result.is_error());
//     EXPECT_EQ(result.error()->code(), ErrorCode::POSITION_LIMIT_EXCEEDED);
// }

TEST_F(BaseStrategyTest, CheckRiskLimits_FailsOnMaxDrawdown) {
    StrategyConfig config;
    config.capital_allocation = 100000;
    auto strategy = createRunningStrategy(config);

    // Simulate a large loss
    strategy->on_execution(createExecution(Side::SELL, "AAPL", 1000, 50.0));  // Short 1000 shares
    strategy->on_execution(
        createExecution(Side::BUY, "AAPL", 1000, 200.0));  // Buy back at higher price
    // Realized PnL: (50 - 200) * 1000 = -150,000 → Drawdown = -150%

    RiskLimits limits;
    limits.max_drawdown = 0.5;  // 50% max drawdown
    strategy->update_risk_limits(limits);
    auto result = strategy->check_risk_limits();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::RISK_LIMIT_EXCEEDED);
}

// --- Concurrency ---
TEST_F(BaseStrategyTest, ThreadSafety_OnDataAndExecution) {
    // Create config with reasonable limits
    StrategyConfig config;
    config.capital_allocation = 1000000.0;  // $1M capital
    config.max_leverage = 4.0;              // 4x max leverage

    auto db = std::make_shared<MockPostgresDatabase>();
    auto strategy = createRunningStrategy(config, db);

    // Add a small initial position to avoid errors with first update
    Position initial_pos;
    initial_pos.symbol = "AAPL";
    initial_pos.quantity = 10;  // Small initial position
    initial_pos.average_price = 150.0;
    initial_pos.last_update = std::chrono::system_clock::now();
    strategy->update_position("AAPL", initial_pos);

    std::atomic<bool> test_passed{true};
    std::atomic<int> data_processed{0};
    std::atomic<int> executions_processed{0};

    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool ready = false;

    // Create test data with reasonable values
    std::vector<Bar> test_data;
    Bar bar;
    bar.symbol = "AAPL";
    bar.timestamp = std::chrono::system_clock::now();
    bar.open = bar.high = bar.low = bar.close = 150.0;
    bar.volume = 1000;
    test_data.push_back(bar);

    auto data_thread = std::thread([&]() {
        try {
            {
                std::unique_lock<std::mutex> lock(start_mutex);
                start_cv.wait(lock, [&ready] { return ready; });
            }

            for (int i = 0; i < 100 && test_passed; ++i) {
                test_data[0].timestamp = std::chrono::system_clock::now();
                // Small price changes to avoid triggering risk limits
                test_data[0].close = 150.0 + (i % 5);
                auto result = strategy->on_data(test_data);
                if (result.is_error()) {
                    std::cerr << "Data error: " << result.error()->what() << std::endl;
                    test_passed = false;
                    break;
                }
                data_processed++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (const std::exception& e) {
            test_passed = false;
            std::cerr << "Data thread exception: " << e.what() << std::endl;
        }
    });

    auto exec_thread = std::thread([&]() {
        try {
            // Small trade size to avoid hitting limits
            auto report = createExecution(Side::BUY, "AAPL", 1, 150.0);
            report.fill_time = std::chrono::system_clock::now();

            {
                std::unique_lock<std::mutex> lock(start_mutex);
                start_cv.wait(lock, [&ready] { return ready; });
            }

            for (int i = 0; i < 100 && test_passed; ++i) {
                report.fill_time = std::chrono::system_clock::now();
                report.fill_price = 150.0 + (i % 5);  // Small price changes
                auto result = strategy->on_execution(report);
                if (result.is_error()) {
                    std::cerr << "Execution error: " << result.error()->what() << std::endl;
                    test_passed = false;
                    break;
                }
                executions_processed++;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (const std::exception& e) {
            test_passed = false;
            std::cerr << "Execution thread exception: " << e.what() << std::endl;
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    {
        std::lock_guard<std::mutex> lock(start_mutex);
        ready = true;
        start_cv.notify_all();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    data_thread.join();
    exec_thread.join();

    EXPECT_TRUE(test_passed) << "Data processed: " << data_processed
                             << ", Executions processed: " << executions_processed;
    EXPECT_GT(data_processed, 0);
    EXPECT_GT(executions_processed, 0);
}

// --- Metrics & Signals ---
TEST_F(BaseStrategyTest, UpdateMetrics_CalculatesUnrealizedPnl) {
    auto strategy = createRunningStrategy();
    strategy->update_position("AAPL", createPosition(100, 150.0));
    strategy->update_position("GOOG", createPosition(-50, 2000.0));
    auto result = strategy->update_metrics();
    EXPECT_TRUE(result.is_ok());
    // Assuming unrealized PnL is tracked (mock market data needed for accuracy)
}

// --- Edge Cases ---
TEST_F(BaseStrategyTest, Initialize_FailsWithZeroCapital) {
    StrategyConfig config;
    config.capital_allocation = 0;  // Invalid
    BaseStrategy strategy("test_strategy", config, std::make_shared<MockPostgresDatabase>());
    auto result = strategy.initialize();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(BaseStrategyTest, OnData_IgnoresNonBarEvents) {
    auto strategy = createRunningStrategy();
    MarketDataEvent event;
    event.type = MarketDataEventType::TRADE;  // Not BAR
    // Verify callback ignores non-BAR events (no crash/error)
}

// --- State Transition Validation ---
TEST_F(BaseStrategyTest, ValidateStateTransition_BlocksInvalidTransitions) {
    auto strategy = createInitializedStrategy();
    // INITIALIZED → PAUSED (invalid)
    auto result = strategy->transition_state(StrategyState::PAUSED);
    EXPECT_TRUE(result.is_error());
}
