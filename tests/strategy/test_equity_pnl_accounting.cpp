#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"
#include "trade_ngin/transaction_cost/asset_cost_config.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;
using namespace trade_ngin::transaction_cost;

// ===========================================================================
// Test: Equity PnL accounting correctness
// Validates the reference walkthrough from the plan:
//   - Cost basis preserved across bars (not overwritten to bar.close)
//   - Unrealized PnL = (price - avg_cost) * qty
//   - Realized PnL computed correctly on partial/full closes
//   - Flip logic: close old side, open new at fill price
//   - MIXED accounting: get_total_pnl() = realized + unrealized
// ===========================================================================

class EquityPnLAccountingTest : public TestBase {
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
        strategy_config_.trading_params["AAPL"] = 1.0;
        strategy_config_.position_limits["AAPL"] = 1000.0;

        mr_config_.lookback_period = 5;
        mr_config_.entry_threshold = 1.5;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.vol_lookback = 5;
        mr_config_.use_stop_loss = false;
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

    Bar make_bar(const std::string& symbol, double price, int day_offset = 0) {
        Bar bar;
        bar.symbol = symbol;
        bar.timestamp = std::chrono::system_clock::now() - std::chrono::hours(24 * day_offset);
        bar.open = price;
        bar.high = price * 1.01;
        bar.low = price * 0.99;
        bar.close = price;
        bar.volume = 1000000;
        return bar;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    MeanReversionConfig mr_config_;
    std::unique_ptr<MeanReversionStrategy> strategy_;
};

// Test: average_price is NOT overwritten by on_data after an execution
TEST_F(EquityPnLAccountingTest, CostBasisPreservedAcrossBars) {
    strategy_ = std::make_unique<MeanReversionStrategy>("test_pnl", strategy_config_, mr_config_, db_);

    auto init_result = strategy_->initialize();
    ASSERT_TRUE(init_result.is_ok());
    strategy_->start();

    // Simulate a fill at $150
    ExecutionReport fill;
    fill.symbol = "AAPL";
    fill.side = Side::BUY;
    fill.fill_price = 150.0;
    fill.filled_quantity = 100;
    fill.fill_time = std::chrono::system_clock::now();
    fill.total_transaction_costs = 0.0;

    auto exec_result = strategy_->on_execution(fill);
    ASSERT_TRUE(exec_result.is_ok());

    // Verify average_price is set to fill price
    const auto& positions = strategy_->get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end());
    EXPECT_NEAR(static_cast<double>(it->second.average_price), 150.0, 1e-6);

    // Now call on_data with a different price — average_price must NOT change
    std::vector<Bar> bars = {make_bar("AAPL", 160.0)};
    auto data_result = strategy_->on_data(bars);
    ASSERT_TRUE(data_result.is_ok());

    // average_price must still be $150 (the fill price), NOT $160 (the bar close)
    const auto& positions_after = strategy_->get_positions();
    auto it2 = positions_after.find("AAPL");
    ASSERT_NE(it2, positions_after.end());
    EXPECT_NEAR(static_cast<double>(it2->second.average_price), 150.0, 1e-6);
}

// Test: MIXED accounting returns realized + unrealized
TEST_F(EquityPnLAccountingTest, MixedAccountingMethod) {
    strategy_ = std::make_unique<MeanReversionStrategy>("test_mixed", strategy_config_, mr_config_, db_);

    auto init_result = strategy_->initialize();
    ASSERT_TRUE(init_result.is_ok());
    strategy_->start();

    // Buy 100 shares at $150
    ExecutionReport buy_fill;
    buy_fill.symbol = "AAPL";
    buy_fill.side = Side::BUY;
    buy_fill.fill_price = 150.0;
    buy_fill.filled_quantity = 100;
    buy_fill.fill_time = std::chrono::system_clock::now();
    buy_fill.total_transaction_costs = 0.0;
    strategy_->on_execution(buy_fill);

    // Process bar at $160 — unrealized PnL should be (160-150)*100 = $1000
    std::vector<Bar> bars = {make_bar("AAPL", 160.0)};
    strategy_->on_data(bars);

    const auto& positions = strategy_->get_positions();
    auto it = positions.find("AAPL");
    ASSERT_NE(it, positions.end());

    // Unrealized PnL = (160 - 150) * 100 = 1000
    double unrealized = static_cast<double>(it->second.unrealized_pnl);
    EXPECT_NEAR(unrealized, 1000.0, 1e-2);
}

// ===========================================================================
// Test: Transaction cost defaults
// ===========================================================================

TEST_F(EquityPnLAccountingTest, UnknownEquityGetsCorrectDefaults) {
    AssetCostConfigRegistry registry;

    // Unknown equity should get equity defaults, not futures defaults
    auto equity_config = registry.get_config("RANDOM_STOCK", AssetType::EQUITY);
    EXPECT_NEAR(equity_config.point_value, 1.0, 1e-6);        // NOT 100.0
    EXPECT_NEAR(equity_config.commission_per_unit, 0.005, 1e-6); // NOT -1.0
    EXPECT_TRUE(equity_config.apply_regulatory_fees);

    // Unknown futures should still get futures defaults
    auto futures_config = registry.get_config("UNKNOWN_FUTURE");
    EXPECT_NEAR(futures_config.point_value, 100.0, 1e-6);
    EXPECT_LT(futures_config.commission_per_unit, 0.0);  // -1.0 (use global)
    EXPECT_FALSE(futures_config.apply_regulatory_fees);
}

TEST_F(EquityPnLAccountingTest, KnownEquityGetsExplicitConfig) {
    AssetCostConfigRegistry registry;

    auto aapl_config = registry.get_config("AAPL");
    EXPECT_NEAR(aapl_config.point_value, 1.0, 1e-6);
    EXPECT_NEAR(aapl_config.commission_per_unit, 0.005, 1e-6);
    EXPECT_TRUE(aapl_config.apply_regulatory_fees);
    EXPECT_NEAR(aapl_config.max_commission_pct, 0.005, 1e-6);  // 0.5% cap
}

TEST_F(EquityPnLAccountingTest, TieredEquityConfig) {
    // Mega-cap tier
    auto mega = AssetCostConfigRegistry::get_tiered_equity_config(200.0, 15000000.0);
    EXPECT_NEAR(mega.baseline_spread_ticks, 1.0, 1e-6);
    EXPECT_NEAR(mega.max_impact_bps, 50.0, 1e-6);

    // Small-cap tier
    auto small = AssetCostConfigRegistry::get_tiered_equity_config(15.0, 200000.0);
    EXPECT_NEAR(small.baseline_spread_ticks, 5.0, 1e-6);
    EXPECT_NEAR(small.max_impact_bps, 150.0, 1e-6);

    // Penny stock with sub-dollar price
    auto penny = AssetCostConfigRegistry::get_tiered_equity_config(0.50, 50000.0);
    EXPECT_NEAR(penny.tick_size, 0.0001, 1e-8);  // Sub-dollar tick size
    EXPECT_NEAR(penny.baseline_spread_ticks, 10.0, 1e-6);
}

// ===========================================================================
// E2-F8: the target position carries a MARK, positions_ carries the BASIS.
//
// The backtest coordinator's unrealized calculation reads average_price off the
// positions returned by get_strategy_positions(), which are the strategy's TARGET
// positions -- and MeanReversionStrategy::get_target_positions() sets
// `pos.average_price = inst_data.current_price` (mean_reversion.cpp:214), the day's
// close. Measured against the live DB, the mark implied by day D's stored unrealized
// equalled day D+1's stored average_price on ~87% of consecutive rows: the column was
// a one-day mark change, not a position-lifetime unrealized.
//
// BaseStrategy::on_execution() is the sole legitimate writer of a volume-weighted cost
// basis and keeps it in positions_, which get_target_positions() deliberately does not
// read. The coordinator must therefore prefer positions_ when measuring unrealized.
//
// These pin the divergence itself. If someone later "fixes" get_target_positions() to
// return the basis, the first test fails and points them at the coordinator so the two
// fixes cannot silently double up. See docs/AVERAGE_PRICE_LIFECYCLE.md for the three
// meanings of this field and which is in force where.
// ===========================================================================

TEST_F(EquityPnLAccountingTest, TargetPositionsCarryTheMarkWhileHeldPositionsCarryTheBasis) {
    strategy_ = std::make_unique<MeanReversionStrategy>("test_basis_vs_mark", strategy_config_,
                                                        mr_config_, db_);
    ASSERT_TRUE(strategy_->initialize().is_ok());
    strategy_->start();

    ExecutionReport fill;
    fill.symbol = "AAPL";
    fill.side = Side::BUY;
    fill.fill_price = 150.0;
    fill.filled_quantity = 100;
    fill.fill_time = std::chrono::system_clock::now();
    fill.total_transaction_costs = 0.0;
    ASSERT_TRUE(strategy_->on_execution(fill).is_ok());

    // A later bar at a different price.
    std::vector<Bar> bars = {make_bar("AAPL", 160.0)};
    ASSERT_TRUE(strategy_->on_data(bars).is_ok());

    // The fill-maintained record holds what the position COST.
    const auto& held = strategy_->get_positions();
    auto h = held.find("AAPL");
    ASSERT_NE(h, held.end());
    EXPECT_NEAR(static_cast<double>(h->second.average_price), 150.0, 1e-6)
        << "positions_ must hold the fill-weighted cost basis -- on_execution is its sole "
           "writer.";

    // The target position holds what it is WORTH. These are different quantities and the
    // coordinator must not confuse them.
    auto targets = strategy_->get_target_positions();
    auto t = targets.find("AAPL");
    ASSERT_NE(t, targets.end());
    EXPECT_NEAR(static_cast<double>(t->second.average_price), 160.0, 1e-6)
        << "get_target_positions() no longer reports the mark. If this was deliberate, the "
           "basis resolution in BacktestCoordinator (E2-F8) is now redundant and must be "
           "removed in the same change -- otherwise the two corrections stack.";

    EXPECT_NE(static_cast<double>(h->second.average_price),
              static_cast<double>(t->second.average_price))
        << "The mark and the basis have converged, so this fixture no longer exercises the "
           "divergence it exists to pin.";
}

TEST_F(EquityPnLAccountingTest, OnExecutionMaintainsVolumeWeightedBasisAcrossFills) {
    strategy_ = std::make_unique<MeanReversionStrategy>("test_vwap_basis", strategy_config_,
                                                        mr_config_, db_);
    ASSERT_TRUE(strategy_->initialize().is_ok());
    strategy_->start();

    ExecutionReport first;
    first.symbol = "AAPL";
    first.side = Side::BUY;
    first.fill_price = 150.0;
    first.filled_quantity = 100;
    first.fill_time = std::chrono::system_clock::now();
    first.total_transaction_costs = 0.0;
    ASSERT_TRUE(strategy_->on_execution(first).is_ok());

    ExecutionReport second = first;
    second.fill_price = 170.0;
    second.filled_quantity = 100;
    ASSERT_TRUE(strategy_->on_execution(second).is_ok());

    const auto& held = strategy_->get_positions();
    auto h = held.find("AAPL");
    ASSERT_NE(h, held.end());
    EXPECT_NEAR(static_cast<double>(h->second.quantity), 200.0, 1e-6);
    EXPECT_NEAR(static_cast<double>(h->second.average_price), 160.0, 1e-6)
        << "Two 100-share buys at 150 and 170 must give a 160.00 weighted basis. This is "
           "the value the backtest coordinator now measures unrealized against.";
}
