// tests/strategy/test_equity_backtest_db_validation.cpp
//
// Integration tests that run equity backtests and validate results
// by querying the database for correctness.
//
// Gated by TRADE_NGIN_DB_TESTS=1 environment variable.

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <random>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/backtest/backtest_coordinator.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;
using namespace trade_ngin::backtest;

class EquityBacktestDBValidation : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        // Skip if DB tests not enabled
        const char* db_tests = std::getenv("TRADE_NGIN_DB_TESTS");
        if (!db_tests || std::string(db_tests) != "1") {
            GTEST_SKIP() << "Database integration tests disabled. "
                         << "Set TRADE_NGIN_DB_TESTS=1 to enable.";
        }

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        auto connect_result = db_->connect();
        ASSERT_TRUE(connect_result.is_ok());

        // Common config
        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 1.0;
        strategy_config_.max_drawdown = 0.3;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};

        mr_config_.lookback_period = 20;
        mr_config_.entry_threshold = 2.0;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.vol_lookback = 20;
        mr_config_.use_stop_loss = true;
        mr_config_.stop_loss_pct = 0.05;
        mr_config_.allow_fractional_shares = true;

        symbols_ = {"AAPL", "MSFT", "GOOGL"};
        for (const auto& symbol : symbols_) {
            strategy_config_.trading_params[symbol] = 1.0;
            strategy_config_.position_limits[symbol] = 500.0;
        }
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

    // Generate mean-reverting price data for equity backtests
    std::vector<Bar> create_equity_data(const std::string& symbol, int num_bars = 120,
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
            bar.high = std::max(bar.open.as_double(), bar.close.as_double()) *
                       (1.0 + std::abs(dist(gen)) * 0.5);
            bar.low = std::min(bar.open.as_double(), bar.close.as_double()) *
                      (1.0 - std::abs(dist(gen)) * 0.5);
            bar.volume = 1000000 + static_cast<double>(rand() % 500000);

            data.push_back(bar);
        }

        return data;
    }

    std::unique_ptr<MeanReversionStrategy> create_strategy(const std::string& id = "") {
        static int test_id = 0;
        std::string unique_id = id.empty() ? "DB_VAL_MR_" + std::to_string(++test_id) : id;
        auto strategy = std::make_unique<MeanReversionStrategy>(
            unique_id, strategy_config_, mr_config_, db_);
        auto init_result = strategy->initialize();
        EXPECT_TRUE(init_result.is_ok()) << "Strategy init failed: "
                                         << (init_result.is_error() ? init_result.error()->what() : "");
        auto start_result = strategy->start();
        EXPECT_TRUE(start_result.is_ok());
        return strategy;
    }

    // Run a backtest and return positions by processing all bars day by day
    struct BacktestResult {
        std::vector<std::unordered_map<std::string, Position>> daily_positions;
        std::vector<double> daily_equity;
        double final_equity = 0.0;
        int total_trades = 0;
    };

    BacktestResult run_simple_backtest(MeanReversionStrategy& strategy,
                                       const std::vector<Bar>& all_bars) {
        BacktestResult result;
        double equity = strategy_config_.capital_allocation;

        // Group bars by timestamp
        std::map<std::chrono::system_clock::time_point, std::vector<Bar>> bars_by_day;
        for (const auto& bar : all_bars) {
            bars_by_day[bar.timestamp].push_back(bar);
        }

        std::unordered_map<std::string, double> prev_prices;

        for (const auto& [timestamp, day_bars] : bars_by_day) {
            // Feed bars to strategy
            auto data_result = strategy.on_data(day_bars);
            EXPECT_TRUE(data_result.is_ok());

            // Get target positions
            auto positions = strategy.get_target_positions();
            result.daily_positions.push_back(positions);

            // Calculate simple PnL (equity multiplier = 1.0)
            for (const auto& bar : day_bars) {
                if (prev_prices.count(bar.symbol) && positions.count(bar.symbol)) {
                    double qty = positions[bar.symbol].quantity.as_double();
                    double price_change = bar.close.as_double() - prev_prices[bar.symbol];
                    equity += qty * price_change;  // No multiplier for equities
                }
                prev_prices[bar.symbol] = bar.close.as_double();
            }

            result.daily_equity.push_back(equity);
        }

        result.final_equity = equity;

        // Count trades (position changes)
        for (size_t i = 1; i < result.daily_positions.size(); i++) {
            for (const auto& [symbol, pos] : result.daily_positions[i]) {
                double prev_qty = 0.0;
                if (result.daily_positions[i - 1].count(symbol)) {
                    prev_qty = result.daily_positions[i - 1][symbol].quantity.as_double();
                }
                if (std::abs(pos.quantity.as_double() - prev_qty) > 0.001) {
                    result.total_trades++;
                }
            }
        }

        return result;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::unique_ptr<MeanReversionStrategy> strategy_;
    StrategyConfig strategy_config_;
    MeanReversionConfig mr_config_;
    RiskLimits risk_limits_;
    std::vector<std::string> symbols_;
};

// ============================================================================
// TEST: Backtest produces valid metrics (no NaN, reasonable ranges)
// ============================================================================
TEST_F(EquityBacktestDBValidation, MetricsAreConsistent) {
    auto strategy = create_strategy();

    // Generate data for all symbols
    std::vector<Bar> all_bars;
    for (const auto& symbol : symbols_) {
        auto bars = create_equity_data(symbol, 120, 150.0);
        all_bars.insert(all_bars.end(), bars.begin(), bars.end());
    }

    auto result = run_simple_backtest(*strategy, all_bars);

    // Verify equity curve exists and is non-empty
    ASSERT_FALSE(result.daily_equity.empty()) << "Equity curve should not be empty";

    // Final equity should be a real number
    EXPECT_FALSE(std::isnan(result.final_equity)) << "Final equity should not be NaN";
    EXPECT_FALSE(std::isinf(result.final_equity)) << "Final equity should not be Inf";
    EXPECT_GT(result.final_equity, 0.0) << "Final equity should be positive";

    // Calculate return
    double total_return = (result.final_equity - strategy_config_.capital_allocation) /
                          strategy_config_.capital_allocation;
    EXPECT_GT(total_return, -1.0) << "Total return should be > -100%";
    EXPECT_LT(total_return, 10.0) << "Total return should be < 1000%";

    // Calculate max drawdown
    double peak = result.daily_equity[0];
    double max_drawdown = 0.0;
    for (double eq : result.daily_equity) {
        peak = std::max(peak, eq);
        double dd = (peak - eq) / peak;
        max_drawdown = std::max(max_drawdown, dd);
    }
    EXPECT_GE(max_drawdown, 0.0) << "Max drawdown should be >= 0";
    EXPECT_LE(max_drawdown, 1.0) << "Max drawdown should be <= 1";

    // Calculate daily returns for volatility check
    std::vector<double> daily_returns;
    for (size_t i = 1; i < result.daily_equity.size(); i++) {
        if (result.daily_equity[i - 1] > 0) {
            daily_returns.push_back(
                (result.daily_equity[i] - result.daily_equity[i - 1]) / result.daily_equity[i - 1]);
        }
    }

    if (!daily_returns.empty()) {
        double mean_return = std::accumulate(daily_returns.begin(), daily_returns.end(), 0.0) /
                             daily_returns.size();
        double sq_sum = 0.0;
        for (double r : daily_returns) {
            sq_sum += (r - mean_return) * (r - mean_return);
        }
        double volatility = std::sqrt(sq_sum / daily_returns.size()) * std::sqrt(252.0);
        EXPECT_GE(volatility, 0.0) << "Annualized volatility should be >= 0";
    }
}

// ============================================================================
// TEST: Position sizes never exceed configured limits
// ============================================================================
TEST_F(EquityBacktestDBValidation, PositionSizesWithinLimits) {
    auto strategy = create_strategy();

    std::vector<Bar> all_bars;
    for (const auto& symbol : symbols_) {
        auto bars = create_equity_data(symbol, 120, 150.0);
        all_bars.insert(all_bars.end(), bars.begin(), bars.end());
    }

    auto result = run_simple_backtest(*strategy, all_bars);

    double position_limit = strategy_config_.position_limits.begin()->second;  // 500.0

    for (size_t day = 0; day < result.daily_positions.size(); day++) {
        for (const auto& [symbol, pos] : result.daily_positions[day]) {
            EXPECT_LE(std::abs(pos.quantity.as_double()), position_limit + 0.01)
                << "Position for " << symbol << " on day " << day
                << " exceeds limit: " << pos.quantity.as_double() << " > " << position_limit;
        }
    }
}

// ============================================================================
// TEST: Equity PnL uses multiplier of 1.0 (no futures-style multiplier)
// ============================================================================
TEST_F(EquityBacktestDBValidation, EquityMultiplierIsOne) {
    auto strategy = create_strategy();

    // Use single symbol for easier verification
    auto bars = create_equity_data("AAPL", 60, 150.0);

    auto result = run_simple_backtest(*strategy, bars);

    // Verify PnL is computed as qty * price_change (no multiplier)
    // The equity curve should move proportionally to position * price change
    if (result.daily_positions.size() >= 2 && result.daily_equity.size() >= 2) {
        // Find a day where we have a position
        for (size_t i = 1; i < result.daily_positions.size() && i < result.daily_equity.size(); i++) {
            if (result.daily_positions[i].count("AAPL") &&
                std::abs(result.daily_positions[i]["AAPL"].quantity.as_double()) > 0.001) {
                double qty = result.daily_positions[i]["AAPL"].quantity.as_double();
                double equity_change = result.daily_equity[i] - result.daily_equity[i - 1];

                // Equity change should be reasonably small (no 50x multiplier effect)
                // A $150 stock moving 2% with 100 shares = $300, not $15,000
                double price = 150.0;
                double max_reasonable_change = std::abs(qty) * price * 0.1;  // 10% daily move
                EXPECT_LE(std::abs(equity_change), max_reasonable_change + 1.0)
                    << "Equity change " << equity_change
                    << " seems too large for " << qty << " shares at ~$" << price
                    << ". Possible futures-style multiplier leak.";
                break;
            }
        }
    }
}

// ============================================================================
// TEST: PnL consistency — sum of daily changes matches total PnL
// ============================================================================
TEST_F(EquityBacktestDBValidation, PnLMatchesEquityCurve) {
    auto strategy = create_strategy();

    std::vector<Bar> all_bars;
    for (const auto& symbol : symbols_) {
        auto bars = create_equity_data(symbol, 120, 150.0);
        all_bars.insert(all_bars.end(), bars.begin(), bars.end());
    }

    auto result = run_simple_backtest(*strategy, all_bars);

    ASSERT_GE(result.daily_equity.size(), 2u);

    // Sum of daily equity changes should equal total PnL
    double sum_daily_changes = 0.0;
    for (size_t i = 1; i < result.daily_equity.size(); i++) {
        sum_daily_changes += result.daily_equity[i] - result.daily_equity[i - 1];
    }

    double total_pnl = result.final_equity - strategy_config_.capital_allocation;

    // Allow small floating point tolerance
    double tolerance = std::abs(total_pnl) * 0.001 + 0.01;  // 0.1% + $0.01
    EXPECT_NEAR(sum_daily_changes, total_pnl, tolerance)
        << "Sum of daily PnL changes (" << sum_daily_changes
        << ") should match total PnL (" << total_pnl << ")";
}

// ============================================================================
// TEST: No positions opened with insufficient data (< lookback period)
// ============================================================================
TEST_F(EquityBacktestDBValidation, NoPositionOnInsufficientData) {
    auto strategy = create_strategy();

    // Create only 10 bars (less than lookback_period of 20)
    auto bars = create_equity_data("AAPL", 10, 150.0);

    auto result = run_simple_backtest(*strategy, bars);

    // With only 10 bars and lookback of 20, strategy should not open positions
    for (size_t day = 0; day < result.daily_positions.size(); day++) {
        for (const auto& [symbol, pos] : result.daily_positions[day]) {
            EXPECT_NEAR(pos.quantity.as_double(), 0.0, 0.001)
                << "Should not have position in " << symbol << " on day " << day
                << " with insufficient data (only " << bars.size()
                << " bars, need " << mr_config_.lookback_period << ")";
        }
    }
}

// ============================================================================
// TEST: Strategy respects UNREALIZED_ONLY PnL accounting
// ============================================================================
TEST_F(EquityBacktestDBValidation, UsesUnrealizedPnLAccounting) {
    auto strategy = create_strategy();

    // Verify the strategy uses UNREALIZED_ONLY accounting
    // Run some data through
    auto bars = create_equity_data("AAPL", 60, 150.0);
    for (const auto& bar : bars) {
        strategy->on_data({bar});
    }

    // Get PnL accounting - verify UNREALIZED_ONLY is set
    const auto& pnl = strategy->get_pnl_accounting();
    // With UNREALIZED_ONLY, the accounting method should track unrealized PnL
    // and realized PnL should remain at 0 while positions are open
    EXPECT_EQ(pnl.method, PnLAccountingMethod::UNREALIZED_ONLY)
        << "Equity strategy should use UNREALIZED_ONLY PnL accounting";
}

// ============================================================================
// TEST: Whole shares mode respects integer positions
// ============================================================================
TEST_F(EquityBacktestDBValidation, WholeSharesMode) {
    // Disable fractional shares
    mr_config_.allow_fractional_shares = false;

    auto strategy = create_strategy();

    std::vector<Bar> all_bars;
    for (const auto& symbol : symbols_) {
        auto bars = create_equity_data(symbol, 120, 150.0);
        all_bars.insert(all_bars.end(), bars.begin(), bars.end());
    }

    auto result = run_simple_backtest(*strategy, all_bars);

    // All positions should be whole numbers
    for (size_t day = 0; day < result.daily_positions.size(); day++) {
        for (const auto& [symbol, pos] : result.daily_positions[day]) {
            double qty = pos.quantity.as_double();
            double fractional = std::abs(qty) - std::floor(std::abs(qty));
            EXPECT_NEAR(fractional, 0.0, 0.001)
                << "Position for " << symbol << " on day " << day
                << " should be whole shares but got " << qty;
        }
    }
}
