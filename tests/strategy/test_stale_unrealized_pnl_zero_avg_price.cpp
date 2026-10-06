#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <random>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Regression test for audit finding §1.8: the unrealized PnL update in
// MeanReversionStrategy::on_data() previously did not guard against
// average_price == 0, so a fresh entry (quantity set by on_data, average_price
// not yet set by on_execution) reported unrealized = bar.close * quantity —
// the full notional — instead of 0.
class StaleUnrealizedPnLTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        strategy_config_.capital_allocation = 100000.0;
        strategy_config_.max_leverage = 2.0;
        strategy_config_.asset_classes = {AssetClass::EQUITIES};
        strategy_config_.frequencies = {DataFrequency::DAILY};
        strategy_config_.trading_params["AAPL"] = 1.0;
        strategy_config_.position_limits["AAPL"] = 100000.0;

        mr_config_.lookback_period = 20;
        mr_config_.vol_lookback = 20;
        mr_config_.entry_threshold = 1.5;
        mr_config_.exit_threshold = 0.5;
        mr_config_.risk_target = 0.15;
        mr_config_.position_size = 0.1;
        mr_config_.allow_fractional_shares = true;

        RiskLimits limits;
        limits.max_position_size = 100000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.9;
        limits.max_leverage = 10.0;

        strategy_ = std::make_unique<MeanReversionStrategy>(
            "TEST_STALE_UNREAL", strategy_config_, mr_config_, db_);
        ASSERT_TRUE(strategy_->initialize().is_ok());
        ASSERT_TRUE(strategy_->update_risk_limits(limits).is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
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

    // Build a synthetic price series that warms up the strategy (so a position
    // can be sized) and then pushes prices far enough from the mean to trigger
    // an entry signal.
    std::vector<Bar> build_warmup_then_breakout(const std::string& symbol) {
        std::vector<Bar> data;
        auto now = std::chrono::system_clock::now();
        // 30 stable bars at $100 to populate lookback windows.
        for (int i = 0; i < 30; ++i) {
            Bar b;
            b.symbol = symbol;
            b.timestamp = now - std::chrono::hours(24 * (40 - i));
            b.open = 100.0;
            b.high = 100.5;
            b.low = 99.5;
            b.close = 100.0;
            b.volume = 1'000'000.0;
            data.push_back(b);
        }
        // 10 bars dropping 5% — wide z-score, should trigger long signal.
        double price = 100.0;
        for (int i = 0; i < 10; ++i) {
            price -= 0.5;
            Bar b;
            b.symbol = symbol;
            b.timestamp = now - std::chrono::hours(24 * (10 - i));
            b.open = price + 0.1;
            b.high = price + 0.2;
            b.low = price - 0.2;
            b.close = price;
            b.volume = 1'000'000.0;
            data.push_back(b);
        }
        return data;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig strategy_config_;
    MeanReversionConfig mr_config_;
    std::unique_ptr<MeanReversionStrategy> strategy_;
};

// Feed bars one-at-a-time (matching live mode where on_data and on_execution
// alternate). Without calling on_execution, any newly-sized position carries
// quantity != 0 with average_price == 0. The fix ensures unrealized_pnl stays
// 0 in that window, not (bar.close * quantity).
TEST_F(StaleUnrealizedPnLTest, FreshEntryReportsZeroUnrealizedNotNotional) {
    auto bars = build_warmup_then_breakout("AAPL");

    for (const auto& bar : bars) {
        std::vector<Bar> single = {bar};
        ASSERT_TRUE(strategy_->on_data(single).is_ok());
        // Do NOT call on_execution -- simulating the window between data and fill.

        const auto& positions = strategy_->get_positions();
        auto it = positions.find("AAPL");
        if (it == positions.end()) continue;
        const Position& pos = it->second;

        // If a non-zero position has been sized but cost basis is still 0,
        // unrealized_pnl must be 0 (not (bar.close - 0) * quantity).
        if (pos.quantity != Decimal(0.0) && pos.average_price == Decimal(0.0)) {
            EXPECT_EQ(pos.unrealized_pnl, Decimal(0.0))
                << "Fresh entry should report 0 unrealized while avg_price is 0; "
                << "got " << pos.unrealized_pnl.as_double()
                << " (qty=" << pos.quantity.as_double()
                << ", close=" << bar.close.as_double() << ")";
        }
    }
}
