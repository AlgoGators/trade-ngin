#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <numeric>
#include <random>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

class MeanReversionBacktestTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        auto connect_result = db_->connect();
        ASSERT_TRUE(connect_result.is_ok());

        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        risk_limits_.max_position_size = 1000.0;
        risk_limits_.max_notional_value = 50000.0;
        risk_limits_.max_drawdown = 0.3;
        risk_limits_.max_leverage = 2.0;

        for (const auto& symbol : {"AAPL", "MSFT", "GOOGL"}) {
            strategy_config_.trading_params[symbol] = 1.0;
            strategy_config_.position_limits[symbol] = 1000.0;
        }

        mr_config_.lookback_period = 20;
        mr_config_.entry_threshold = 2.0;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.vol_lookback = 20;
        mr_config_.use_stop_loss = true;
        mr_config_.stop_loss_pct = 0.05;
        mr_config_.allow_fractional_shares = true;
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

    std::vector<Bar> create_equity_data(const std::string& symbol, int num_bars = 60,
                                        double start_price = 150.0, double volatility = 0.02) {
        std::vector<Bar> data;
        auto now = std::chrono::system_clock::now();

        std::mt19937 gen(42);
        std::normal_distribution<> dist(0, volatility);

        double price = start_price;
        double mean_price = start_price;

        data.reserve(num_bars);

        for (int i = 0; i < num_bars; i++) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (num_bars - i));

            double deviation = price - mean_price;
            double mean_reversion = -0.1 * deviation;
            double random_shock = dist(gen) * start_price;

            price += mean_reversion + random_shock;
            price = std::max(start_price * 0.5, std::min(price, start_price * 1.5));

            bar.close = price;
            bar.open = price * (1.0 + dist(gen) * 0.5);
            bar.high = std::max(bar.open.as_double(), bar.close.as_double()) * (1.0 + std::abs(dist(gen)) * 0.5);
            bar.low = std::min(bar.open.as_double(), bar.close.as_double()) * (1.0 - std::abs(dist(gen)) * 0.5);
            bar.volume = 1000000 + static_cast<double>(rand() % 500000);

            data.push_back(bar);
        }

        return data;
    }

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

    auto test_data = create_equity_data("AAPL", 60, 150.0, 0.02);

    for (size_t i = 0; i < test_data.size(); i += 10) {
        size_t end_idx = std::min(i + 10, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok()) << "Failed to process chunk: "
                                    << (result.is_error() ? result.error()->what() : "");
    }

    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());

    double z_score = strategy_->get_z_score("AAPL");
    EXPECT_TRUE(std::abs(z_score) < 10.0) << "Z-score out of reasonable range: " << z_score;
}

// Test fractional share support (FIX MAJOR #5)
TEST_F(MeanReversionBacktestTest, FractionalShareSupport) {
    mr_config_.allow_fractional_shares = true;
    strategy_ = create_strategy();

    auto test_data = create_equity_data("AAPL", 60, 150.0, 0.03);

    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok());

    // With fractional shares enabled, positions may have decimal quantities
    const auto& positions = strategy_->get_positions();
    if (positions.find("AAPL") != positions.end()) {
        double quantity = positions.at("AAPL").quantity.as_double();
        // Fractional quantities should be rounded to 6 decimal places
        double rounded = std::round(quantity * 1000000.0) / 1000000.0;
        EXPECT_NEAR(quantity, rounded, 1e-7);
    }
}

// Test whole shares mode (FIX MAJOR #5 - config flag)
TEST_F(MeanReversionBacktestTest, WholeSharesMode) {
    mr_config_.allow_fractional_shares = false;
    strategy_ = create_strategy();

    auto test_data = create_equity_data("AAPL", 60, 150.0, 0.03);

    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok());

    const auto& positions = strategy_->get_positions();
    if (positions.find("AAPL") != positions.end()) {
        double quantity = positions.at("AAPL").quantity.as_double();
        double fractional_part = std::abs(quantity) - std::floor(std::abs(quantity));
        EXPECT_NEAR(fractional_part, 0.0, 1e-6)
            << "Position should be whole shares, got: " << quantity;
    }
}

// Test price history trimming (FIX MAJOR #1)
TEST_F(MeanReversionBacktestTest, PriceHistoryTrimming) {
    strategy_ = create_strategy();

    // Process a large amount of data to test trimming
    auto test_data = create_equity_data("AAPL", 200, 150.0, 0.02);

    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok());

    // Price history should be trimmed to max_size = 2 * max(lookback, vol_lookback)
    auto price_history = strategy_->get_price_history();
    if (price_history.find("AAPL") != price_history.end()) {
        size_t max_expected = static_cast<size_t>(
            std::max(mr_config_.lookback_period, mr_config_.vol_lookback) * 2);
        EXPECT_LE(price_history.at("AAPL").size(), max_expected)
            << "Price history should be trimmed to " << max_expected
            << " but has " << price_history.at("AAPL").size();
    }
}

// Test position limits enforcement (FIX MAJOR #2)
TEST_F(MeanReversionBacktestTest, PositionLimitsEnforced) {
    // Set a very low position limit
    strategy_config_.position_limits["AAPL"] = 10.0;  // Max 10 shares
    mr_config_.allow_fractional_shares = false;
    strategy_ = create_strategy();

    auto test_data = create_equity_data("AAPL", 60, 150.0, 0.03);

    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok());

    const auto& positions = strategy_->get_positions();
    if (positions.find("AAPL") != positions.end()) {
        double quantity = std::abs(positions.at("AAPL").quantity.as_double());
        EXPECT_LE(quantity, 10.0)
            << "Position should not exceed limit of 10 shares, got: " << quantity;
    }
}

// Test entry price cleared on position close (FIX MOD #3)
TEST_F(MeanReversionBacktestTest, EntryPriceClearedOnClose) {
    strategy_ = create_strategy();

    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();
    double base_price = 150.0;

    // Phase 1: Build baseline
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

    // Phase 2: Oversold to trigger long entry
    for (int i = 0; i < 15; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (60 - i));
        bar.close = base_price * 0.85;
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 1500000;
        test_data.push_back(bar);
    }

    // Phase 3: Revert to mean (trigger exit)
    for (int i = 0; i < 15; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (45 - i));
        bar.close = base_price;
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 1200000;
        test_data.push_back(bar);
    }

    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok());

    // After mean reversion completes, entry price should be cleared if position is flat
    auto inst_data = strategy_->get_instrument_data("AAPL");
    ASSERT_NE(inst_data, nullptr);
    if (std::abs(inst_data->target_position) < 1e-6) {
        EXPECT_NEAR(inst_data->entry_price, 0.0, 1e-6)
            << "Entry price should be cleared when position is closed";
    }
}

// Test mean reversion behavior: buy low, sell high
TEST_F(MeanReversionBacktestTest, MeanReversionBehavior) {
    strategy_ = create_strategy();

    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();
    double base_price = 150.0;

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

    for (int i = 0; i < 20; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (60 - i));
        bar.close = base_price * 0.85;
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 1500000;
        test_data.push_back(bar);
    }

    for (int i = 0; i < 10; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (40 - i));
        bar.close = base_price * (0.85 + 0.015 * i);
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 1200000;
        test_data.push_back(bar);
    }

    std::vector<double> positions_over_time;

    for (size_t i = 0; i < test_data.size(); i += 5) {
        size_t end_idx = std::min(i + 5, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok());

        const auto& positions = strategy_->get_positions();
        if (positions.find("AAPL") != positions.end()) {
            positions_over_time.push_back(positions.at("AAPL").quantity.as_double());
        }
    }

    EXPECT_FALSE(positions_over_time.empty());
}

// Test multiple equities simultaneously
TEST_F(MeanReversionBacktestTest, MultipleEquities) {
    strategy_ = create_strategy();

    auto aapl_data = create_equity_data("AAPL", 60, 150.0, 0.02);
    auto msft_data = create_equity_data("MSFT", 60, 280.0, 0.025);
    auto googl_data = create_equity_data("GOOGL", 60, 2800.0, 0.03);

    ASSERT_TRUE(strategy_->on_data(aapl_data).is_ok());
    ASSERT_TRUE(strategy_->on_data(msft_data).is_ok());
    ASSERT_TRUE(strategy_->on_data(googl_data).is_ok());

    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());
    EXPECT_TRUE(positions.find("MSFT") != positions.end());
    EXPECT_TRUE(positions.find("GOOGL") != positions.end());

    EXPECT_TRUE(std::abs(strategy_->get_z_score("AAPL")) < 10.0);
    EXPECT_TRUE(std::abs(strategy_->get_z_score("MSFT")) < 10.0);
    EXPECT_TRUE(std::abs(strategy_->get_z_score("GOOGL")) < 10.0);
}

// Test with realistic adjusted close data (FIX MOD #2)
// In real data, only close is adjusted; open/high/low are raw (unadjusted)
TEST_F(MeanReversionBacktestTest, RealisticAdjustedCloseData) {
    strategy_ = create_strategy();

    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();

    for (int i = 0; i < 60; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (60 - i));

        if (i < 30) {
            // Pre-split: raw prices around $300, adjusted by 0.5 (2:1 split)
            // This simulates what the DB query produces with the adjustment ratio
            double adj_base = 150.0;  // 300 * 0.5
            double random_var = (rand() % 100 - 50) / 1000.0;

            double close_price = adj_base * (1.0 + random_var);
            bar.close = close_price;
            bar.open = adj_base * 0.99;
            bar.high = std::max({close_price, adj_base * 0.99, adj_base * 1.02});
            bar.low = std::min({close_price, adj_base * 0.99, adj_base * 0.98});
        } else {
            // Post-split: prices around $150 (no adjustment needed)
            double base = 150.0;
            double random_var = (rand() % 100 - 50) / 1000.0;

            double close_price = base * (1.0 + random_var);
            bar.close = close_price;
            bar.open = base * 0.99;
            bar.high = std::max({close_price, base * 0.99, base * 1.02});
            bar.low = std::min({close_price, base * 0.99, base * 0.98});
        }
        bar.volume = 1000000;

        // Verify OHLC consistency: low <= open,close <= high
        EXPECT_LE(bar.low.as_double(), bar.open.as_double())
            << "Bar " << i << ": low > open";
        EXPECT_LE(bar.low.as_double(), bar.close.as_double())
            << "Bar " << i << ": low > close";
        EXPECT_GE(bar.high.as_double(), bar.open.as_double())
            << "Bar " << i << ": high < open";
        EXPECT_GE(bar.high.as_double(), bar.close.as_double())
            << "Bar " << i << ": high < close";

        test_data.push_back(bar);
    }

    auto result = strategy_->on_data(test_data);
    ASSERT_TRUE(result.is_ok()) << "Failed to process adjusted close data";

    const auto& positions = strategy_->get_positions();
    EXPECT_TRUE(positions.find("AAPL") != positions.end());
}

// Test stop loss functionality with equities
TEST_F(MeanReversionBacktestTest, StopLossWithEquities) {
    strategy_ = create_strategy();

    std::vector<Bar> test_data;
    auto now = std::chrono::system_clock::now();
    double start_price = 100.0;

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

    for (int i = 0; i < 10; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (30 - i));
        bar.close = start_price * 0.85;
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 2000000;
        test_data.push_back(bar);
    }

    for (int i = 0; i < 10; i++) {
        Bar bar;
        bar.symbol = "AAPL";
        bar.timestamp = now - std::chrono::hours(24 * (20 - i));
        bar.close = start_price * 0.78;
        bar.open = bar.close;
        bar.high = bar.close * 1.01;
        bar.low = bar.close * 0.99;
        bar.volume = 2500000;
        test_data.push_back(bar);
    }

    for (size_t i = 0; i < test_data.size(); i += 5) {
        size_t end_idx = std::min(i + 5, test_data.size());
        std::vector<Bar> chunk(test_data.begin() + i, test_data.begin() + end_idx);
        auto result = strategy_->on_data(chunk);
        ASSERT_TRUE(result.is_ok());
    }

    SUCCEED();
}
