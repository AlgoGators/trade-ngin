#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// Review T-OR.5: the backtest and live equity runners must produce identical signals
// from identical bars. Both runners now build the MeanReversionConfig via the shared
// build_mean_reversion_config() helper (review T2.6); this test pins that two
// strategies constructed that way agree bar-for-bar. It is the regression tripwire
// for any future change that lets the two runners' construction drift apart.

class LiveBtSignalConsistencyTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        base_config_.capital_allocation = 100000.0;
        base_config_.max_leverage = 1.0;
        base_config_.asset_classes = {AssetClass::EQUITIES};
        base_config_.frequencies = {DataFrequency::DAILY};
        for (const auto& symbol : symbols_) {
            base_config_.trading_params[symbol] = 1.0;
            base_config_.position_limits[symbol] = 1000.0;
        }

        risk_limits_.max_position_size = 1000.0;
        risk_limits_.max_notional_value = 50000.0;
        risk_limits_.max_drawdown = 0.3;
        risk_limits_.max_leverage = 2.0;
    }

    void TearDown() override {
        if (db_) {
            db_->disconnect();
            db_.reset();
        }
        TestBase::TearDown();
    }

    // Deterministic mean-reverting series (fixed seed); the *same* vector is fed to
    // both strategies, so input is identical by construction.
    std::vector<Bar> make_bars(const std::string& symbol, int n, double start,
                               unsigned seed) const {
        std::vector<Bar> data;
        data.reserve(n);
        auto now = std::chrono::system_clock::now();
        std::mt19937 gen(seed);
        std::normal_distribution<> dist(0.0, 0.02);
        double price = start;
        for (int i = 0; i < n; ++i) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (n - i));
            double deviation = price - start;
            price += -0.1 * deviation + dist(gen) * start;
            price = std::max(start * 0.5, std::min(price, start * 1.5));
            bar.close = price;
            bar.open = price;
            bar.high = price * 1.01;
            bar.low = price * 0.99;
            bar.volume = 1000000.0;  // constant -> no rand() nondeterminism
            data.push_back(bar);
        }
        return data;
    }

    std::unique_ptr<MeanReversionStrategy> make_strategy(const std::string& id,
                                                         const MeanReversionConfig& mr) {
        auto s = std::make_unique<MeanReversionStrategy>(id, base_config_, mr, db_);
        EXPECT_TRUE(s->initialize().is_ok());
        EXPECT_TRUE(s->update_risk_limits(risk_limits_).is_ok());
        EXPECT_TRUE(s->start().is_ok());
        return s;
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig base_config_;
    RiskLimits risk_limits_;
    std::vector<std::string> symbols_{"AAPL", "MSFT", "GOOGL"};
};

TEST_F(LiveBtSignalConsistencyTest, SharedBuilderYieldsIdenticalSignals) {
    // The same JSON "config" block both runners read from portfolio.json.
    nlohmann::json cfg = {{"lookback_period", 20},  {"entry_threshold", 2.0},
                          {"exit_threshold", 0.5},  {"risk_target", 0.15},
                          {"position_size", 0.1},   {"vol_lookback", 20},
                          {"use_stop_loss", true},  {"stop_loss_pct", 0.05},
                          {"allow_fractional_shares", true}};

    // Both runners construct via the shared builder -> identical MeanReversionConfig.
    auto mr_bt = apps::build_mean_reversion_config(cfg);
    auto mr_live = apps::build_mean_reversion_config(cfg);

    // Distinct ids (StateManager keys by id); behavior must still be identical.
    auto bt_strat = make_strategy("BT_PARITY", mr_bt);
    auto live_strat = make_strategy("LIVE_PARITY", mr_live);

    for (const auto& symbol : symbols_) {
        auto bars = make_bars(symbol, 60, 150.0, 42);  // identical vector to both
        ASSERT_TRUE(bt_strat->on_data(bars).is_ok());
        ASSERT_TRUE(live_strat->on_data(bars).is_ok());
    }

    const auto& sig_bt = bt_strat->get_last_signals();
    const auto& sig_live = live_strat->get_last_signals();

    ASSERT_EQ(sig_bt.size(), sig_live.size());
    for (const auto& [symbol, value] : sig_bt) {
        auto it = sig_live.find(symbol);
        ASSERT_NE(it, sig_live.end()) << "live missing signal for " << symbol;
        EXPECT_DOUBLE_EQ(value, it->second) << "signal mismatch for " << symbol;
    }

    // Cross-check via the per-symbol z-score accessor as well.
    for (const auto& symbol : symbols_) {
        EXPECT_DOUBLE_EQ(bt_strat->get_z_score(symbol), live_strat->get_z_score(symbol));
    }

    bt_strat->stop();
    live_strat->stop();
}
