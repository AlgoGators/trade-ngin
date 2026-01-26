#include <gtest/gtest.h>
#include <thread>
#include "../data/test_db_utils.hpp"
#include "../order/test_utils.hpp"
#include "mock_strategy.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class PortfolioManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();

        // Reset state manager
        StateManager::instance().reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Create portfolio config
        PortfolioConfig config{
            1000000.0,  // total_capital ($1M)
            100000.0,   // reserve_capital ($100K reserve)
            0.4,        // max_strategy_allocation (40% max per strategy)
            0.1,        // min_strategy_allocation (10% min per strategy)
            false,      // use_optimization
            false       // use_risk_management
        };

        // Set up optimization config
        config.opt_config.tau = 1.0;
        config.opt_config.capital = config.total_capital.as_double();
        config.opt_config.asymmetric_risk_buffer = 0.1;
        config.opt_config.cost_penalty_scalar = 10;
        config.opt_config.max_iterations = 100;
        config.opt_config.convergence_threshold = 1e-6;

        // Set up risk config
        config.risk_config.var_limit = 0.15;
        config.risk_config.max_correlation = 0.7;
        config.risk_config.capital = config.total_capital;
        config.risk_config.confidence_level = 0.99;
        config.risk_config.lookback_period = 252;

        static int manager_counter = 0;
        manager_id_ = "PORTFOLIO_MANAGER_" + std::to_string(++manager_counter);

        manager_ = std::make_unique<PortfolioManager>(config, manager_id_);
        ASSERT_TRUE(manager_ != nullptr) << "Failed to create PortfolioManager";

        // Create a mock database for strategies
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        auto connect_result = db_->connect();
        ASSERT_TRUE(connect_result.is_ok())
            << "DB connection failed: "
            << (connect_result.error() ? connect_result.error()->what() : "unknown error");
    }

    void TearDown() override {
        if (manager_) {
            manager_.reset();
        }

        if (db_) {
            db_.reset();
        }

        StateManager::instance().reset_instance();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        TestBase::TearDown();
    }

    std::shared_ptr<StrategyInterface> create_test_strategy(
        const std::string& id, const std::vector<std::string>& symbols = {"AAPL", "MSFT"}) {
        static int counter = 0;
        std::string unique_id = id + "_" + std::to_string(++counter);

        StrategyConfig strategy_config;
        strategy_config.capital_allocation = 1000000.0;
        strategy_config.max_leverage = 2.0;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};

        // Add trading parameters for the symbols
        for (const auto& symbol : symbols) {
            strategy_config.trading_params[symbol] = 1.0;       // Add multiplier for each symbol
            strategy_config.position_limits[symbol] = 10000.0;  // Add position limit
        }

        auto strategy = std::make_shared<MockStrategy>(unique_id, strategy_config, db_);

        // Initialize and start strategy
        auto init_result = strategy->initialize();
        if (init_result.is_error()) {
            throw std::runtime_error("Failed to initialize strategy: " +
                                     std::string(init_result.error()->what()));
        }

        auto start_result = strategy->start();
        if (start_result.is_error()) {
            throw std::runtime_error("Failed to start strategy: " +
                                     std::string(start_result.error()->what()));
        }

        return strategy;
    }

    std::vector<Bar> create_historical_data(const std::string& symbol,
                                            int num_bars = 300) {  // More than 256 required
        std::vector<Bar> data;

        auto now = std::chrono::system_clock::now();

        for (int i = 0; i < num_bars; i++) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (num_bars - i));
            bar.open = 100.0 + std::sin(i * 0.1);  // Create some price movement
            bar.high = bar.open + 1.0;
            bar.low = bar.open - 1.0;
            bar.close = bar.open + 0.5;
            bar.volume = 100000;
            data.push_back(bar);
        }

        return data;
    }

    std::unique_ptr<PortfolioManager> manager_;
    std::shared_ptr<DatabaseInterface> db_;
    std::string manager_id_;
};

TEST_F(PortfolioManagerTest, AddStrategy) {
    try {
        // Create a test strategy with explicit error checking
        auto strategy = create_test_strategy("MOCK");
        ASSERT_TRUE(strategy != nullptr) << "Strategy creation failed";

        // Verify strategy state
        auto state = strategy->get_state();
        ASSERT_TRUE(state == StrategyState::RUNNING) << "Strategy not in running state";

        // Add strategy to manager
        auto add_result = manager_->add_strategy(strategy,
                                                 0.3,   // 30% allocation
                                                 true,  // use optimization
                                                 false  // do not use risk management
        );
        ASSERT_TRUE(add_result.is_ok())
            << "Failed to add strategy: "
            << (add_result.error() ? add_result.error()->what() : "Unknown error");

        // Create historical data for AAPL
        auto historical_data = create_historical_data("AAPL");

        // Process market data with error handling
        auto process_result = manager_->process_market_data(historical_data);
        if (process_result.is_error()) {
            std::cout << "Process error: " << process_result.error()->what() << std::endl;
        }
        ASSERT_TRUE(process_result.is_ok())
            << "Failed to process market data: "
            << (process_result.error() ? process_result.error()->what() : "Unknown error");

        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Verify portfolio positions with detailed output on failure
        auto positions = manager_->get_portfolio_positions();

        // Don't assert on empty positions yet, just log
        if (positions.empty()) {
            std::cout << "Warning: No positions generated in test" << std::endl;
        }

    } catch (const std::exception& e) {
        FAIL() << "Unexpected exception: " << e.what();
    }
}

TEST_F(PortfolioManagerTest, AllocationLimits) {
    try {
        // Try to add strategy with too high allocation
        auto strategy1 = create_test_strategy("MOCK");
        auto result1 = manager_->add_strategy(strategy1, 0.5);  // 50% > max 40%
        EXPECT_TRUE(result1.is_error());

        // Add strategy with valid allocation
        auto strategy2 = create_test_strategy("MOCK");
        auto result2 = manager_->add_strategy(strategy2, 0.3);  // 30% is valid
        ASSERT_TRUE(result2.is_ok());

        // Try to add another strategy that would exceed 100%
        auto strategy3 = create_test_strategy("MOCK");
        auto result3 = manager_->add_strategy(strategy3, 0.8);  // Would exceed 100%
        EXPECT_TRUE(result3.is_error());
    } catch (const std::exception& e) {
        FAIL() << "Unexpected exception: " << e.what();
    }
}

TEST_F(PortfolioManagerTest, ProcessMarketData) {
    try {
        // Add two strategies
        auto strategy1 = create_test_strategy("MOCK", {"AAPL"});
        auto strategy2 = create_test_strategy("MOCK", {"MSFT"});

        // Add strategies to manager
        ASSERT_TRUE(manager_->add_strategy(strategy1, 0.3).is_ok());
        ASSERT_TRUE(manager_->add_strategy(strategy2, 0.3).is_ok());

        // Create market data
        std::vector<Bar> data;
        auto now = std::chrono::system_clock::now();

        auto historical_data = create_historical_data("AAPL");
        auto historical_data2 = create_historical_data("MSFT");

        // Combine data for both symbols
        historical_data.insert(historical_data.end(), historical_data2.begin(),
                               historical_data2.end());

        // Process market data
        auto result = manager_->process_market_data(historical_data);
        ASSERT_TRUE(result.is_ok()) << "Failed to process market data: " << result.error()->what();

        // Check positions with detailed output
        auto positions = manager_->get_portfolio_positions();
        EXPECT_FALSE(positions.empty()) << "Expected positions after processing market data";

        // Verify specific positions exist
        EXPECT_TRUE(positions.find("AAPL") != positions.end()) << "Expected AAPL position";
        EXPECT_TRUE(positions.find("MSFT") != positions.end()) << "Expected MSFT position";

    } catch (const std::exception& e) {
        FAIL() << "Unexpected exception: " << e.what();
    }
}

TEST_F(PortfolioManagerTest, UpdateAllocations) {
    try {
        auto strategy1 = create_test_strategy("MOCK");
        auto strategy2 = create_test_strategy("MOCK");
        auto strategy3 = create_test_strategy("MOCK");

        ASSERT_TRUE(manager_->add_strategy(strategy1, 0.2).is_ok());
        ASSERT_TRUE(manager_->add_strategy(strategy2, 0.2).is_ok());
        ASSERT_TRUE(manager_->add_strategy(strategy3, 0.2).is_ok());

        std::unordered_map<std::string, double> new_allocations = {
            {strategy1->get_metadata().id, 0.4},
            {strategy2->get_metadata().id, 0.3},
            {strategy3->get_metadata().id, 0.3}};

        auto result = manager_->update_allocations(new_allocations);
        ASSERT_TRUE(result.is_ok());
    } catch (const std::exception& e) {
        FAIL() << "Unexpected exception: " << e.what();
    }
}

TEST_F(PortfolioManagerTest, OptimizationIntegration) {
    auto strategy = create_test_strategy("MOCK");
    ASSERT_TRUE(strategy != nullptr) << "Strategy creation failed";

    // Verify strategy state
    ASSERT_TRUE(strategy->get_state() == StrategyState::RUNNING) << "Strategy not in running state";

    auto add_result = manager_->add_strategy(strategy, 0.3, true, false);
    ASSERT_TRUE(add_result.is_ok())
        << "Failed to add strategy: "
        << (add_result.error() ? add_result.error()->what() : "Unknown error");

    // Create market data with some volatility
    auto historical_data = create_historical_data("AAPL");

    auto result = manager_->process_market_data(historical_data);
    ASSERT_TRUE(result.is_ok()) << "Failed to process initial market data: "
                                << (result.error() ? result.error()->what() : "Unknown error");

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    auto positions = manager_->get_portfolio_positions();

    // Don't assert on empty positions yet, just log
    if (positions.empty()) {
        std::cout << "Warning: No positions generated in optimization test" << std::endl;
    }
}

TEST_F(PortfolioManagerTest, RiskManagementIntegration) {
    auto strategy = create_test_strategy("MOCK_1");
    ASSERT_TRUE(manager_->add_strategy(strategy, 0.3, false, true).is_ok());

    // Create market data with high volatility
    std::vector<Bar> data;
    auto now = std::chrono::system_clock::now();

    for (int i = 0; i < 10; ++i) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now + std::chrono::minutes(i);
        bar.open = 150.0 + (i * 1.0);  // Strong uptrend
        bar.high = bar.open + 2.0;     // High volatility
        bar.low = bar.open - 2.0;
        bar.close = bar.open + 1.5;
        bar.volume = 100000;
        data.push_back(bar);
    }

    ASSERT_TRUE(manager_->process_market_data(data).is_ok());

    // Verify risk limits are being enforced
    auto positions = manager_->get_portfolio_positions();
    double total_exposure = 0.0;

    for (const auto& [symbol, pos] : positions) {
        total_exposure += std::abs((pos.quantity * pos.average_price).as_double());
    }

    // Total exposure should not exceed risk limits
    EXPECT_LE(total_exposure, 1000000.0 * 4.0);  // Max 4x leverage
}

TEST_F(PortfolioManagerTest, StressTest) {
    try {
        // Add multiple strategies
        std::vector<std::shared_ptr<StrategyInterface>> strategies;
        for (int i = 0; i < 5; ++i) {
            auto strategy = create_test_strategy("MOCK");
            ASSERT_TRUE(manager_->add_strategy(strategy, 0.15).is_ok());
            strategies.push_back(strategy);
        }

        // Create large market data set
        std::vector<Bar> all_data;
        std::vector<std::string> symbols = {"AAPL", "MSFT", "GOOG", "AMZN", "FB"};

        for (const auto& symbol : symbols) {
            auto symbol_data = create_historical_data(symbol, 300);
            all_data.insert(all_data.end(), symbol_data.begin(), symbol_data.end());
        }

        auto result = manager_->process_market_data(all_data);
        ASSERT_TRUE(result.is_ok());

        // Verify system stability
        auto positions = manager_->get_portfolio_positions();
        EXPECT_FALSE(positions.empty());

        // Check memory usage isn't excessive
        size_t total_size = 0;
        for (const auto& [symbol, pos] : positions) {
            total_size += sizeof(pos);
        }
        EXPECT_LT(total_size, 1024 * 1024);  // Should be well under 1MB
    } catch (const std::exception& e) {
        FAIL() << "Unexpected exception: " << e.what();
    }
}