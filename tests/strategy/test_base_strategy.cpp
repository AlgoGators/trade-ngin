#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
#include <thread>
#include "trade_ngin/core/state_manager.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/types.hpp"

#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/backtest/backtest_pnl_manager.hpp"
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
                                  const std::string& strategy_id, const std::string& strategy_name,
                                  const std::string& portfolio_id,
                                  const std::string& table_name) override {
        (void)strategy_id;
        (void)strategy_name;
        (void)portfolio_id;
        (void)table_name;
        executions_stored = executions;
        return Result<void>();
    }
    Result<void> store_positions(const std::vector<Position>& positions, const std::string&,
                                 const std::string&, const std::string&,
                                 const std::string&) override {
        positions_stored = positions;
        return Result<void>();
    }
    Result<void> store_signals(const std::unordered_map<std::string, double>& signals,
                               const std::string&, const std::string&, const std::string&,
                               const Timestamp&, const std::string&) override {
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

TEST_F(BaseStrategyTest, Stop_TransitionsStateAndRetainsPositions) {
    auto db = std::make_shared<MockPostgresDatabase>();

    StrategyConfig config;
    auto strategy = createRunningStrategy(config, db);

    // Add a test position
    Position pos;
    pos.symbol = "TEST";
    pos.quantity = 100;
    ASSERT_TRUE(strategy->update_position("TEST", pos).is_ok());

    // Stop strategy and verify state transition
    auto result = strategy->stop();
    EXPECT_TRUE(result.is_ok());
    EXPECT_EQ(strategy->get_state(), StrategyState::STOPPED);

    // Positions should still be accessible in memory after stop
    const auto& positions = strategy->get_positions();
    EXPECT_TRUE(positions.find("TEST") != positions.end());
    EXPECT_EQ(positions.at("TEST").quantity, 100);
}

// --- Database & Error Handling ---
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

TEST_F(BaseStrategyTest, OnExecution_UpdatesPositionAndMetrics) {
    auto db = std::make_shared<MockPostgresDatabase>();

    StrategyConfig config;
    auto strategy = createRunningStrategy(config, db);

    // Execute a buy order
    auto report = createExecution(Side::BUY, "AAPL", 100, 150.0);
    auto result = strategy->on_execution(report);
    EXPECT_TRUE(result.is_ok());

    // Verify position was updated in memory
    const auto& positions = strategy->get_positions();
    ASSERT_TRUE(positions.find("AAPL") != positions.end());
    EXPECT_EQ(positions.at("AAPL").quantity, 100);
    EXPECT_DOUBLE_EQ(positions.at("AAPL").average_price.as_double(), 150.0);
}

// --- Position & Risk Limits ---
TEST_F(BaseStrategyTest, UpdatePosition_FailsIfExceedsLimit) {
    StrategyConfig config;
    config.position_limits["AAPL"] = 100;
    auto strategy = createRunningStrategy(config);
    Position pos;
    pos.quantity = 200;  // Exceeds limit
    auto result = strategy->update_position("AAPL", pos);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::POSITION_LIMIT_EXCEEDED);
}

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

// ===== folded in from tests/strategy/test_pnl_accounting_branch.cpp =====
namespace pnl_accounting_branch_detail {

using namespace trade_ngin;
using namespace trade_ngin::testing;
// This file also declares a file-scope MockPostgresDatabase; the folded-in
// tests were written against trade_ngin::testing's. Pin that resolution.
using MockPostgresDatabase = trade_ngin::testing::MockPostgresDatabase;

// Phase 4 audit test T4.6 — §1.14 backtest coordinator P&L semantics.
//
// Contract test: the coordinator's branch reads strategy->get_pnl_accounting().method
// and only stamps realized_pnl when REALIZED_ONLY (futures: settled daily).
// MIXED / UNREALIZED_ONLY (equities) leaves realized_pnl untouched at the
// coordinator level -- on_execution writes realized when positions actually close.
//
// This test pins the contract by verifying:
// 1. Each strategy's accounting method is what the coordinator expects.
// 2. The PnL accounting accessor returns the same value over successive calls.
//
// A true integration test of the coordinator's branch requires the full bar
// loop, portfolio, and execution path -- captured by the broader smoke runs
// rather than a unit test. This contract test catches regressions in the
// accessor / setter contract that the coordinator's branch depends on.

namespace {

class PnLAccountingBranchTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        db_ = std::make_shared<MockPostgresDatabase>("mock://pnl_branch_test");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    std::shared_ptr<MockPostgresDatabase> db_;
};

}  // namespace

TEST_F(PnLAccountingBranchTest, MeanReversionUsesMixed) {
    StrategyConfig cfg;
    cfg.capital_allocation = 100000.0;
    cfg.max_leverage = 2.0;
    cfg.max_drawdown = 0.3;
    cfg.asset_classes = {AssetClass::EQUITIES};
    cfg.frequencies = {DataFrequency::DAILY};
    cfg.trading_params["AAPL"] = 1.0;
    cfg.position_limits["AAPL"] = 1000.0;

    MeanReversionConfig mr;
    mr.lookback_period = 20;
    mr.vol_lookback = 20;
    mr.entry_threshold = 2.0;
    mr.exit_threshold = 0.5;
    mr.risk_target = 0.15;
    mr.position_size = 0.1;

    MeanReversionStrategy strat("TEST_MR_PNL", cfg, mr, db_);
    ASSERT_TRUE(strat.initialize().is_ok());

    EXPECT_EQ(strat.get_pnl_accounting().method, PnLAccountingMethod::MIXED)
        << "Equity mean reversion must declare MIXED accounting so the "
           "backtest coordinator skips daily realized_pnl writes (Phase 4 §1.14).";
}

TEST_F(PnLAccountingBranchTest, TrendFollowingUsesRealizedOnly) {
    StrategyConfig cfg;
    cfg.capital_allocation = 100000.0;
    cfg.max_leverage = 2.0;
    cfg.max_drawdown = 0.3;
    cfg.asset_classes = {AssetClass::FUTURES};
    cfg.frequencies = {DataFrequency::DAILY};
    cfg.trading_params["ES"] = 1.0;
    cfg.position_limits["ES"] = 10.0;

    TrendFollowingConfig tfc;

    TrendFollowingStrategy strat("TEST_TF_PNL", cfg, tfc, db_);
    ASSERT_TRUE(strat.initialize().is_ok());

    EXPECT_EQ(strat.get_pnl_accounting().method, PnLAccountingMethod::REALIZED_ONLY)
        << "Futures trend following must declare REALIZED_ONLY accounting so "
           "the backtest coordinator writes daily MTM into realized_pnl "
           "(futures settle daily) per Phase 4 §1.14.";
}

TEST_F(PnLAccountingBranchTest, AccessorIsStableAcrossCalls) {
    StrategyConfig cfg;
    cfg.capital_allocation = 100000.0;
    cfg.max_leverage = 2.0;
    cfg.max_drawdown = 0.3;
    cfg.asset_classes = {AssetClass::EQUITIES};
    cfg.frequencies = {DataFrequency::DAILY};
    cfg.trading_params["AAPL"] = 1.0;
    cfg.position_limits["AAPL"] = 1000.0;

    MeanReversionStrategy strat("TEST_MR_STABLE", cfg, MeanReversionConfig{}, db_);
    ASSERT_TRUE(strat.initialize().is_ok());

    auto method_a = strat.get_pnl_accounting().method;
    auto method_b = strat.get_pnl_accounting().method;
    auto method_c = strat.get_pnl_accounting().method;
    EXPECT_EQ(method_a, method_b);
    EXPECT_EQ(method_b, method_c);
}

}  // namespace pnl_accounting_branch_detail

// ============================================================================
// E2-F27 / T-OR.4: a fill that crosses zero realizes on the CLOSED quantity only.
//
// Long 100 @ 150, SELL 150 @ 170: 100 shares close (realized 100 x 20 = 2000)
// and the remaining 50 open a new short at the fill price. Realizing on the
// full 150 (3000) books P&L on 50 shares that were never held. Mirror for the
// short side. TF/TFF/TFS override on_execution; MR and any non-overriding
// strategy hit this path on an optimizer-driven flip.
// ============================================================================
TEST_F(BaseStrategyTest, OnExecution_FlipRealizesOnlyTheClosedQuantity_Long) {
    auto db = std::make_shared<MockPostgresDatabase>();
    StrategyConfig config;
    auto strategy = createRunningStrategy(config, db);

    ASSERT_TRUE(strategy->on_execution(createExecution(Side::BUY, "AAPL", 100, 150.0)).is_ok());
    ASSERT_TRUE(strategy->on_execution(createExecution(Side::SELL, "AAPL", 150, 170.0)).is_ok());

    const auto& pos = strategy->get_positions().at("AAPL");
    EXPECT_DOUBLE_EQ(pos.realized_pnl.as_double(), 2000.0)
        << "realized must be (170-150) x the 100 shares that closed, not x 150";
    EXPECT_DOUBLE_EQ(strategy->get_metrics().realized_pnl, 2000.0);
    EXPECT_DOUBLE_EQ(pos.quantity.as_double(), -50.0);
    EXPECT_DOUBLE_EQ(pos.average_price.as_double(), 170.0)
        << "the 50-share remainder opens at the fill price";
}

TEST_F(BaseStrategyTest, OnExecution_FlipRealizesOnlyTheClosedQuantity_Short) {
    auto db = std::make_shared<MockPostgresDatabase>();
    StrategyConfig config;
    auto strategy = createRunningStrategy(config, db);

    ASSERT_TRUE(strategy->on_execution(createExecution(Side::SELL, "AAPL", 100, 150.0)).is_ok());
    ASSERT_TRUE(strategy->on_execution(createExecution(Side::BUY, "AAPL", 150, 130.0)).is_ok());

    const auto& pos = strategy->get_positions().at("AAPL");
    EXPECT_DOUBLE_EQ(pos.realized_pnl.as_double(), 2000.0)
        << "realized must be (150-130) x the 100 shares that covered, not x 150";
    EXPECT_DOUBLE_EQ(strategy->get_metrics().realized_pnl, 2000.0);
    EXPECT_DOUBLE_EQ(pos.quantity.as_double(), 50.0);
    EXPECT_DOUBLE_EQ(pos.average_price.as_double(), 130.0)
        << "the 50-share remainder opens at the fill price";
}

// An exact close (qty == fill) and a partial close are unchanged by the fix.
TEST_F(BaseStrategyTest, OnExecution_ExactAndPartialCloseRealizeOnTheFill) {
    auto db = std::make_shared<MockPostgresDatabase>();
    StrategyConfig config;
    auto strategy = createRunningStrategy(config, db);

    ASSERT_TRUE(strategy->on_execution(createExecution(Side::BUY, "AAPL", 100, 150.0)).is_ok());
    ASSERT_TRUE(strategy->on_execution(createExecution(Side::SELL, "AAPL", 40, 160.0)).is_ok());
    EXPECT_DOUBLE_EQ(strategy->get_positions().at("AAPL").realized_pnl.as_double(), 400.0);
    EXPECT_DOUBLE_EQ(strategy->get_positions().at("AAPL").quantity.as_double(), 60.0);
    EXPECT_DOUBLE_EQ(strategy->get_positions().at("AAPL").average_price.as_double(), 150.0);

    ASSERT_TRUE(strategy->on_execution(createExecution(Side::SELL, "AAPL", 60, 170.0)).is_ok());
    EXPECT_DOUBLE_EQ(strategy->get_positions().at("AAPL").realized_pnl.as_double(), 400.0 + 1200.0);
    EXPECT_DOUBLE_EQ(strategy->get_positions().at("AAPL").quantity.as_double(), 0.0);
}

// ---------------------------------------------------------------------------
// C-5 §9-A2 -- the §1.14 branch itself, not just the accessor it reads.
//
// THE TEST DEFECT: the three tests above assert what get_pnl_accounting() returns for a
// mean-reversion and a trend-following config. That contract predates 7e3d07c2. Executed
// revert (C-5 L-CLOSURE): with src/backtest/backtest_coordinator.cpp reverted to 7e3d07c2^
// -- the file whose branch the commit's own header names -- all three still PASS:
//
//     [==========] 3 tests from 1 test suite ran.
//     [  PASSED  ] 3 tests.
//
// The coordinator's branch was never entered, so nothing pinned the behaviour the commit
// shipped. These add it: the two methods must produce DIFFERENT realized figures from the
// same bar, because they are different quantities.
// ---------------------------------------------------------------------------

TEST(PnLAccountingBranchRule, TheTwoMethodsBookDifferentRealizedFromTheSameBar) {
    // One bar: a settled MTM move of -877.50 (the MYM.v.0 figure), and fills that realized
    // +200.00 on this bar.
    const double daily_mtm = -877.50;
    const double flow = 200.00;

    const double futures = trade_ngin::backtest::BacktestPnLManager::realized_for_row(
        PnLAccountingMethod::REALIZED_ONLY, daily_mtm, flow);
    const double equities = trade_ngin::backtest::BacktestPnLManager::realized_for_row(
        PnLAccountingMethod::MIXED, daily_mtm, flow);

    EXPECT_DOUBLE_EQ(futures, daily_mtm)
        << "under REALIZED_ONLY the settled move IS the day's realized";
    EXPECT_DOUBLE_EQ(equities, flow)
        << "under MIXED realized comes from the fills, never from the mark";
    EXPECT_NE(futures, equities)
        << "if these agreed the branch would be unobservable and the column meaningless";
}

TEST(PnLAccountingBranchRule, AHeldEquityDayBooksZeroRealizedNotTheMarkMove) {
    // The defect's signature: a day on which nothing closed. Under MIXED the row must read
    // 0.00, not the day's mark-to-market move -- otherwise every held day looks like a
    // realizing day and the column no longer sums to the position's realized P&L.
    const double realized = trade_ngin::backtest::BacktestPnLManager::realized_for_row(
        PnLAccountingMethod::MIXED, /*daily_mtm=*/-877.50, /*flow=*/0.0);
    EXPECT_DOUBLE_EQ(realized, 0.0);
}

TEST(PnLAccountingBranchRule, UnrealizedOnlyBooksTheFlowLikeMixed) {
    // UNREALIZED_ONLY is a cash book too; only REALIZED_ONLY takes the mark.
    EXPECT_DOUBLE_EQ(trade_ngin::backtest::BacktestPnLManager::realized_for_row(
                         PnLAccountingMethod::UNREALIZED_ONLY, -877.50, 200.0),
                     200.0);
}
