#include <gtest/gtest.h>
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "../core/test_base.hpp"
#include "../order/test_utils.hpp"
#include "../data/test_db_utils.hpp"
#include <memory>

using namespace trade_ngin;
using namespace trade_ngin::testing;

class PortfolioManagerTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        
        // Create portfolio config
        PortfolioConfig config;
        config.total_capital = 1000000.0;    // $1M
        config.reserve_capital = 100000.0;    // $100K reserve
        config.max_strategy_allocation = 0.4;  // 40% max per strategy
        config.min_strategy_allocation = 0.1;  // 10% min per strategy
        config.use_optimization = true;
        config.use_risk_management = true;

        // Set up optimization config
        config.opt_config.tau = 1.0;
        config.opt_config.capital = config.total_capital;
        config.opt_config.asymmetric_risk_buffer = 0.1;
        config.opt_config.cost_penalty_scalar = 10;

        // Set up risk config
        config.risk_config.var_limit = 0.15;
        config.risk_config.max_correlation = 0.7;
        config.risk_config.capital = config.total_capital;

        manager_ = std::make_unique<PortfolioManager>(config);

        // Create a mock database for strategies
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }

    std::shared_ptr<StrategyInterface> create_test_strategy(
        const std::string& id,
        const std::vector<std::string>& symbols = {"AAPL", "MSFT"}) {
        
        StrategyConfig strategy_config;
        strategy_config.capital_allocation = 1000000.0;
        strategy_config.max_leverage = 2.0;
        strategy_config.asset_classes = {AssetClass::EQUITIES};
        strategy_config.frequencies = {DataFrequency::DAILY};
        
        TrendFollowingConfig trend_config;
        trend_config.risk_target = 0.2;
        trend_config.idm = 2.5;
        trend_config.use_position_buffering = true;
        trend_config.ema_windows = {{2, 8}, {4, 16}};

        return std::make_shared<TrendFollowingStrategy>(
            id, strategy_config, trend_config, db_);
    }

    std::unique_ptr<PortfolioManager> manager_;
    std::shared_ptr<DatabaseInterface> db_;
};

TEST_F(PortfolioManagerTest, AddStrategy) {
    auto strategy = create_test_strategy("TREND_1");
    
    auto result = manager_->add_strategy(
        strategy,
        0.3,  // 30% allocation
        true, // use optimization
        true  // use risk management
    );

    ASSERT_TRUE(result.is_ok());

    // Verify portfolio positions are tracking the strategy
    auto positions = manager_->get_portfolio_positions();
    EXPECT_FALSE(positions.empty());
}

TEST_F(PortfolioManagerTest, AllocationLimits) {
    // Try to add strategy with too high allocation
    auto strategy1 = create_test_strategy("TREND_1");
    auto result1 = manager_->add_strategy(strategy1, 0.5); // 50% > max 40%
    EXPECT_TRUE(result1.is_error());

    // Add strategy with valid allocation
    auto strategy2 = create_test_strategy("TREND_2");
    auto result2 = manager_->add_strategy(strategy2, 0.3); // 30% is valid
    ASSERT_TRUE(result2.is_ok());

    // Try to add another strategy that would exceed 100%
    auto strategy3 = create_test_strategy("TREND_3");
    auto result3 = manager_->add_strategy(strategy3, 0.8); // Would exceed 100%
    EXPECT_TRUE(result3.is_error());
}

TEST_F(PortfolioManagerTest, ProcessMarketData) {
    // Add two strategies
    auto strategy1 = create_test_strategy("TREND_1", {"AAPL"});
    auto strategy2 = create_test_strategy("TREND_2", {"MSFT"});
    
    ASSERT_TRUE(manager_->add_strategy(strategy1, 0.3).is_ok());
    ASSERT_TRUE(manager_->add_strategy(strategy2, 0.3).is_ok());

    // Create some market data
    std::vector<Bar> data;
    auto now = std::chrono::system_clock::now();
    
    // AAPL data
    Bar aapl_bar;
    aapl_bar.symbol = "AAPL";
    aapl_bar.timestamp = now;
    aapl_bar.open = 150.0;
    aapl_bar.high = 151.0;
    aapl_bar.low = 149.0;
    aapl_bar.close = 150.5;
    aapl_bar.volume = 1000000;
    data.push_back(aapl_bar);

    // MSFT data
    Bar msft_bar;
    msft_bar.symbol = "MSFT";
    msft_bar.timestamp = now;
    msft_bar.open = 300.0;
    msft_bar.high = 301.0;
    msft_bar.low = 299.0;
    msft_bar.close = 300.5;
    msft_bar.volume = 800000;
    data.push_back(msft_bar);

    // Process market data
    auto result = manager_->process_market_data(data);
    ASSERT_TRUE(result.is_ok());

    // Check that positions were updated
    auto positions = manager_->get_portfolio_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());
    EXPECT_TRUE(positions.find("MSFT") != positions.end());
}

TEST_F(PortfolioManagerTest, UpdateAllocations) {
    // Add strategies
    auto strategy1 = create_test_strategy("TREND_1");
    auto strategy2 = create_test_strategy("TREND_2");
    auto strategy3 = create_test_strategy("TREND_3");
    
    ASSERT_TRUE(manager_->add_strategy(strategy1, 0.2).is_ok());
    ASSERT_TRUE(manager_->add_strategy(strategy2, 0.2).is_ok());
    ASSERT_TRUE(manager_->add_strategy(strategy3, 0.2).is_ok());

    // Update allocations
    std::unordered_map<std::string, double> new_allocations = {
        {"TREND_1", 0.3},
        {"TREND_2", 0.25},
        {"TREND_3", 0.25}
    };

    auto result = manager_->update_allocations(new_allocations);
    ASSERT_TRUE(result.is_ok());

    // Verify position scaling
    auto positions = manager_->get_portfolio_positions();
    auto changes = manager_->get_required_changes();
    EXPECT_FALSE(changes.empty());
}

TEST_F(PortfolioManagerTest, OptimizationIntegration) {
    auto strategy = create_test_strategy("TREND_1");
    ASSERT_TRUE(manager_->add_strategy(strategy, 0.3, true, false).is_ok());

    // Create market data with some volatility
    std::vector<Bar> data;
    auto now = std::chrono::system_clock::now();
    
    for (int i = 0; i < 10; ++i) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now + std::chrono::minutes(i);
        bar.open = 150.0 + (i * 0.1);
        bar.high = bar.open + 0.5;
        bar.low = bar.open - 0.5;
        bar.close = bar.open + 0.2;
        bar.volume = 100000;
        data.push_back(bar);
    }

    ASSERT_TRUE(manager_->process_market_data(data).is_ok());

    // Check that positions are being optimized
    auto positions = manager_->get_portfolio_positions();
    EXPECT_FALSE(positions.empty());
    
    // Positions should be scaled according to optimization constraints
    for (const auto& [symbol, pos] : positions) {
        EXPECT_LE(std::abs(pos.quantity * pos.average_price), 
                 1000000.0);  // Should not exceed capital
    }
}

TEST_F(PortfolioManagerTest, RiskManagementIntegration) {
    auto strategy = create_test_strategy("TREND_1");
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
        total_exposure += std::abs(pos.quantity * pos.average_price);
    }

    // Total exposure should not exceed risk limits
    EXPECT_LE(total_exposure, 1000000.0 * 4.0);  // Max 4x leverage
}

TEST_F(PortfolioManagerTest, StressTest) {
    // Add multiple strategies
    for (int i = 1; i <= 5; ++i) {
        auto strategy = create_test_strategy("TREND_" + std::to_string(i));
        ASSERT_TRUE(manager_->add_strategy(strategy, 0.15).is_ok());
    }

    // Create large market data set
    std::vector<Bar> data;
    auto now = std::chrono::system_clock::now();
    std::vector<std::string> symbols = {"AAPL", "MSFT", "GOOG", "AMZN", "FB"};
    
    for (const auto& symbol : symbols) {
        for (int i = 0; i < 100; ++i) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now + std::chrono::minutes(i);
            bar.open = 100.0 + (i * 0.1);
            bar.high = bar.open + 0.5;
            bar.low = bar.open - 0.5;
            bar.close = bar.open + 0.2;
            bar.volume = 100000;
            data.push_back(bar);
        }
    }

    // Process large amount of market data
    auto result = manager_->process_market_data(data);
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
}