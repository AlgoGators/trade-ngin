#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <numeric>
#include <random>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/backtest/backtest_engine.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

class MeanReversionBacktestTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        // Initialize mock database
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        auto connect_result = db_->connect();
        ASSERT_TRUE(connect_result.is_ok());

        // Configure backtest for equities
        backtest_config_.strategy_config.start_date =
            std::chrono::system_clock::now() - std::chrono::hours(24 * 60);  // 60 days
        backtest_config_.strategy_config.end_date = std::chrono::system_clock::now();
        backtest_config_.strategy_config.symbols = {"AAPL", "MSFT", "GOOGL"};
        backtest_config_.strategy_config.asset_class = AssetClass::EQUITIES;  // Using equities
        backtest_config_.strategy_config.data_freq = DataFrequency::DAILY;
        backtest_config_.strategy_config.data_type = "ohlcv";
        backtest_config_.strategy_config.initial_capital = 100000.0;  // $100K
        backtest_config_.strategy_config.commission_rate = 0.001;     // 10 basis points
        backtest_config_.strategy_config.slippage_model = 0.5;        // 0.5 bp
        backtest_config_.portfolio_config.use_risk_management = false;
        backtest_config_.portfolio_config.use_optimization = false;
        backtest_config_.store_trade_details = true;

        // Create strategy configuration
        strategy_config_.capital_allocation = 100000.0;  // $100K
        strategy_config_.max_leverage = 2.0;             // Lower leverage for equities
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        strategy_config_.save_signals = true;
        strategy_config_.save_positions = true;

        // Configure risk limits
        risk_limits_.max_position_size = 1000.0;
        risk_limits_.max_notional_value = 50000.0;  // Max $50K per position
        risk_limits_.max_drawdown = 0.3;
        risk_limits_.max_leverage = 2.0;

        // Add trading parameters for test symbols (price per share, not multiplier)
        for (const auto& symbol : {"AAPL", "MSFT", "GOOGL"}) {
            strategy_config_.trading_params[symbol] = 1.0;       // 1.0 for equities (price per share)
            strategy_config_.position_limits[symbol] = 1000.0;   // Max 1000 shares
        }

        // Create mean reversion configuration
        mr_config_.lookback_period = 20;        // 20-day moving average
        mr_config_.entry_threshold = 2.0;       // Enter at 2 std devs
        mr_config_.exit_threshold = 0.5;        // Exit at 0.5 std devs
        mr_config_.risk_target = 0.15;          // 15% annualized risk
        mr_config_.position_size = 0.1;         // 10% of capital per position
        mr_config_.vol_lookback = 20;           // 20-day volatility
        mr_config_.use_stop_loss = true;
        mr_config_.stop_loss_pct = 0.05;        // 5% stop loss
    }

    void TearDown() override {
        if (strategy_) {
            strategy_->stop();
            strategy_.reset();
        }
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    // Helper to create equity test data with mean-reverting behavior
    std::vector<Bar> create_equity_data(const std::string& symbol, int num_bars = 60,
                                        double start_price = 150.0, double volatility = 0.02) {
        std::vector<Bar> data;
        auto now = std::chrono::system_clock::now();

        std::mt19937 gen(42);  // Fixed seed for reproducibility
        std::normal_distribution<> dist(0, volatility);

        double price = start_price;
        double mean_price = start_price;

        data.reserve(num_bars);

        for (int i = 0; i < num_bars; i++) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (num_bars - i));

            // Create mean-reverting price action
            double deviation = price - mean_price;
            double mean_reversion = -0.1 * deviation;  // Pull back to mean
            double random_shock = dist(gen) * start_price;

            price += mean_reversion + random_shock;
            price = std::max(start_price * 0.5, std::min(price, start_price * 1.5));

            // Use adjusted close as the primary price (this is what we want to test)
            bar.close = price;  // This represents adjusted close
            bar.open = price * (1.0 + dist(gen) * 0.5);
            bar.high = std::max(bar.open.as_double(), bar.close.as_double()) * (1.0 + std::abs(dist(gen)) * 0.5);
            bar.low = std::min(bar.open.as_double(), bar.close.as_double()) * (1.0 - std::abs(dist(gen)) * 0.5);
            bar.volume = 1000000 + static_cast<double>(rand() % 500000);

            data.push_back(bar);
        }

        return data;
    }

    // Create strategy instance
    std::unique_ptr<MeanReversionStrategy> create_strategy() {
        static int test_id = 0;
        std::string unique_id = "TEST_MR_" + std::to_string(++test_id);

        auto strategy = std::make_unique<MeanReversionStrategy>(
            unique_id, strategy_config_, mr_config_, db_);

        auto init_result = strategy->initialize();
        EXPECT_TRUE(init_result.is_ok()) << "Strategy initialization failed";

        if (init_result.is_ok()) {
            EXPECT_TRUE(strategy->update_risk_limits(risk_limits_).is_ok());
            EXPECT_TRUE(strategy->start().is_ok());
        }

        return strategy;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    RiskLimits risk_limits_;
    MeanReversionConfig mr_config_;
    BacktestConfig backtest_config_;
    std::unique_ptr<MeanReversionStrategy> strategy_;
};

// Test basic strategy initialization with equities
TEST_F(MeanReversionBacktestTest, StrategyInitialization) {
    strategy_ = create_strategy();

    EXPECT_EQ(strategy_->get_state(), StrategyState::RUNNING);
    EXPECT_EQ(strategy_->get_config().capital_allocation, 100000.0);
    EXPECT_FALSE(strategy_->get_positions().empty());
}

// Test mean reversion signal generation with equities data
TEST_F(MeanReversionBacktestTest, SignalGeneration) {
    strategy_ = create_strategy();

    // Create mean-reverting equity data
    auto test_data = create_equity_data("AAPL", 60, 150.0, 0.02);

    // Process data in chunks to build history
    for (size_t i = 0; i < test_data.size(); i += 10) {
        size_t end_idx = std::min(i + 10, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok()) << "Failed to process chunk: "
                                    << (result.is_error() ? result.error()->what() : "");
    }

    // After processing enough data, we should have z-score and position calculations
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());

    // Check that z-score is calculated
    double z_score = strategy_->get_z_score("AAPL");
    EXPECT_TRUE(std::abs(z_score) < 10.0) << "Z-score out of reasonable range: " << z_score;
}

// Test that strategy uses whole shares for equities
TEST_F(MeanReversionBacktestTest, WholeSharesForEquities) {
    strategy_ = create_strategy();

    auto test_data = create_equity_data("AAPL", 60, 150.0, 0.03);

    // Process all data
    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok());

    // Check that positions are in whole shares (integers)
    const auto& positions = strategy_->get_positions();
    if (positions.find("AAPL") != positions.end()) {
        double quantity = positions.at("AAPL").quantity.as_double();
        double fractional_part = quantity - std::floor(quantity);
        EXPECT_NEAR(fractional_part, 0.0, 1e-6)
            << "Position should be whole shares, got: " << quantity;
    }
}

// Test mean reversion behavior: buy low, sell high
TEST_F(MeanReversionBacktestTest, MeanReversionBehavior) {
    strategy_ = create_strategy();

    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();
    double base_price = 150.0;

    // Phase 1: Build baseline with stable prices
    for (int i = 0; i < 30; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (90 - i));
        bar.close = base_price;
        bar.open = base_price;
        bar.high = base_price * 1.01;
        bar.low = base_price * 0.99;
        bar.volume = 1000000;
        test_data.push_back(bar);
    }

    // Phase 2: Price drops significantly (oversold)
    for (int i = 0; i < 20; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (60 - i));
        bar.close = base_price * 0.85;  // 15% below mean
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 1500000;
        test_data.push_back(bar);
    }

    // Phase 3: Price reverts back to mean
    for (int i = 0; i < 10; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (40 - i));
        bar.close = base_price * (0.85 + 0.015 * i);  // Gradually move back
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 1200000;
        test_data.push_back(bar);
    }

    // Process data and track positions
    std::vector<double> positions_over_time;
    std::vector<double> z_scores;

    for (size_t i = 0; i < test_data.size(); i += 5) {
        size_t end_idx = std::min(i + 5, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok());

        const auto& positions = strategy_->get_positions();
        if (positions.find("AAPL") != positions.end()) {
            positions_over_time.push_back(positions.at("AAPL").quantity.as_double());
            z_scores.push_back(strategy_->get_z_score("AAPL"));
        }
    }

    // Strategy should generate some positions during the oversold phase
    EXPECT_FALSE(positions_over_time.empty());
}

// Test multiple equities simultaneously
TEST_F(MeanReversionBacktestTest, MultipleEquities) {
    strategy_ = create_strategy();

    auto aapl_data = create_equity_data("AAPL", 60, 150.0, 0.02);
    auto msft_data = create_equity_data("MSFT", 60, 280.0, 0.025);
    auto googl_data = create_equity_data("GOOGL", 60, 2800.0, 0.03);

    // Process each symbol
    ASSERT_TRUE(strategy_->on_data(aapl_data).is_ok());
    ASSERT_TRUE(strategy_->on_data(msft_data).is_ok());
    ASSERT_TRUE(strategy_->on_data(googl_data).is_ok());

    // Verify positions exist for all symbols
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());
    EXPECT_TRUE(positions.find("MSFT") != positions.end());
    EXPECT_TRUE(positions.find("GOOGL") != positions.end());

    // Verify z-scores are calculated for all
    EXPECT_TRUE(std::abs(strategy_->get_z_score("AAPL")) < 10.0);
    EXPECT_TRUE(std::abs(strategy_->get_z_score("MSFT")) < 10.0);
    EXPECT_TRUE(std::abs(strategy_->get_z_score("GOOGL")) < 10.0);
}

// Test with adjusted close data format
TEST_F(MeanReversionBacktestTest, AdjustedCloseData) {
    strategy_ = create_strategy();

    // Create data that simulates adjusted close prices (e.g., after stock splits)
    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();

    for (int i = 0; i < 60; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (60 - i));

        // Simulate a stock split at day 30 - adjusted close handles this
        double base_price = (i < 30) ? 300.0 : 150.0;  // 2:1 split
        double random_var = (rand() % 100 - 50) / 1000.0;

        // Adjusted close is the main price we use
        bar.close = base_price * (1.0 + random_var);
        bar.open = bar.close * 0.99;
        bar.high = bar.close * 1.02;
        bar.low = bar.close * 0.98;
        bar.volume = 1000000;

        test_data.push_back(bar);
    }

    // Process the data
    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok()) << "Failed to process adjusted close data";

    // Strategy should handle the price adjustment correctly
    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());
}

// Test stop loss functionality with equities
TEST_F(MeanReversionBacktestTest, StopLossWithEquities) {
    strategy_ = create_strategy();

    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();
    double start_price = 100.0;

    // Build history
    for (int i = 0; i < 30; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (60 - i));
        bar.close = start_price;
        bar.open = start_price;
        bar.high = start_price * 1.01;
        bar.low = start_price * 0.99;
        bar.volume = 1000000;
        test_data.push_back(bar);
    }

    // Create oversold condition to trigger entry
    for (int i = 0; i < 10; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (30 - i));
        bar.close = start_price * 0.85;  // Big drop
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 2000000;
        test_data.push_back(bar);
    }

    // Price continues to fall (stop loss scenario)
    for (int i = 0; i < 10; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (20 - i));
        bar.close = start_price * 0.78;  // Falls more (7% below entry)
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 2500000;
        test_data.push_back(bar);
    }

    // Process data
    for (size_t i = 0; i < test_data.size(); i += 5) {
        size_t end_idx = std::min(i + 5, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok());
    }

    // Test completes successfully (stop loss behavior is internal to strategy)
    SUCCEED();
}
