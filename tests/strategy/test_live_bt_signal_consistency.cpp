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
#include "trade_ngin/live/live_daily_cycle.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// NEW-6(a) / T-OR.5 -- the backtest and the live runner must reach the same state from
// the same bars.
//
// This file used to feed BOTH sides the same vector in the same call and assert they
// agreed, which is a tautology: it proved that one function is deterministic. It could
// not see the difference that actually existed, because the two runners do not stage
// their data the same way.
//
//   BACKTEST  BacktestCoordinator walks the history and calls on_data() once per day
//             with that day's bars (backtest_coordinator.cpp:393, :587), then reads
//             get_target_positions().
//   LIVE      LiveDailyCycle::prepare_strategy_for_signals() seeds the previous day's
//             book, then PortfolioManager::process_market_data() calls on_data() once
//             with the whole loaded window (portfolio_manager.cpp:217) and reads
//             get_target_positions().
//
// Until E2-F28 the live path fed that window TWICE -- prepare_strategy_for_signals()
// called on_data() itself and then the portfolio manager called it again. MeanReversion
// appends per bar, so the live strategy's volatility window, ADV EMA and warm-up counter
// were all computed over a series twice as long as the one that traded, and the two
// runners silently disagreed on the same data.
//
// So the test drives the two SEQUENCES and compares the state each arrives at. The
// window is deliberately shorter than vol_lookback: that is where the doubling changes
// the answer rather than merely the bookkeeping, because the volatility window then
// spans the seam between the two copies and the warm-up gate flips.

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

    // The config block both runners read from portfolio.json, through the shared builder
    // (review T2.6). vol_lookback exceeds the window on purpose -- see the file header.
    static nlohmann::json config_json() {
        return {{"lookback_period", 20},  {"entry_threshold", 2.0},
                {"exit_threshold", 0.5},  {"risk_target", 0.15},
                {"position_size", 0.1},   {"vol_lookback", 40},
                {"use_stop_loss", true},  {"stop_loss_pct", 0.05},
                {"allow_fractional_shares", true}};
    }

    // Deterministic mean-reverting series with a VOLUME LEVEL SHIFT, so the ADV EMA is a
    // function of how many times the series was fed and not just of its last value.
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
            bar.volume = (i < n / 2) ? 5000000.0 : 120000.0;
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

    // A held book, so generate_signal() takes its EXIT branch and the stop-loss has a
    // basis to measure from. A fresh flat strategy never executes either, which is why
    // the previous version of this test could not reach the branches that matter.
    std::unordered_map<std::string, Position> held_book() const {
        std::unordered_map<std::string, Position> book;
        for (const auto& symbol : symbols_) {
            Position p;
            p.symbol = symbol;
            p.quantity = Quantity(25.0);
            p.average_price = Decimal(148.0);
            p.last_update = std::chrono::system_clock::now();
            book[symbol] = p;
        }
        return book;
    }

    // BacktestCoordinator's staging: one on_data() per day, in date order.
    static void feed_backtest_sequence(BaseStrategy& strategy,
                                       const std::vector<Bar>& bars) {
        for (const auto& bar : bars) {
            ASSERT_TRUE(strategy.on_data({bar}).is_ok());
        }
    }

    // The live runner's staging: seed the book, then ONE feed of the loaded window by
    // PortfolioManager::process_market_data.
    static void feed_live_sequence(BaseStrategy& strategy,
                                   const std::unordered_map<std::string, Position>& book,
                                   const std::vector<Bar>& bars) {
        ASSERT_TRUE(LiveDailyCycle::prepare_strategy_for_signals(strategy, book).is_ok());
        ASSERT_TRUE(strategy.on_data(bars).is_ok());
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig base_config_;
    RiskLimits risk_limits_;
    std::vector<std::string> symbols_{"AAPL", "MSFT", "GOOGL"};
    static constexpr int kBars = 30;  // shorter than vol_lookback, on purpose
};

// The real parity claim: run the two staging sequences over the same bars with the same
// held book and compare everything the strategy carries into its targets.
TEST_F(LiveBtSignalConsistencyTest, BacktestAndLiveStagingReachTheSameState) {
    auto mr_bt = apps::build_mean_reversion_config(config_json());
    auto mr_live = apps::build_mean_reversion_config(config_json());
    ASSERT_EQ(mr_bt.vol_lookback, 40);
    ASSERT_GT(mr_bt.vol_lookback, kBars)
        << "the window must be shorter than vol_lookback or a doubled feed is invisible";

    auto bt_strat = make_strategy("BT_PARITY", mr_bt);
    auto live_strat = make_strategy("LIVE_PARITY", mr_live);

    const auto book = held_book();
    // The backtest reaches a held book through on_execution over its own history; here
    // both sides are handed the same book so the comparison isolates the STAGING.
    ASSERT_TRUE(bt_strat->seed_positions(book).is_ok());

    for (const auto& symbol : symbols_) {
        const auto bars = make_bars(symbol, kBars, 150.0, 42);  // identical vector to both
        feed_backtest_sequence(*bt_strat, bars);
        feed_live_sequence(*live_strat, book, bars);
    }

    for (const auto& symbol : symbols_) {
        const auto* bt = bt_strat->get_instrument_data(symbol);
        const auto* live = live_strat->get_instrument_data(symbol);
        ASSERT_NE(bt, nullptr);
        ASSERT_NE(live, nullptr);

        EXPECT_EQ(live->price_history.size(), bt->price_history.size())
            << symbol << ": the live staging saw a different number of closes";
        EXPECT_EQ(live->volume_sample_count, bt->volume_sample_count)
            << symbol << ": the ADV warm-up counter drives fractional-share eligibility";
        EXPECT_DOUBLE_EQ(live->avg_daily_volume, bt->avg_daily_volume)
            << symbol << ": the ADV EMA advanced a different number of times";
        EXPECT_DOUBLE_EQ(live->current_volatility, bt->current_volatility)
            << symbol << ": volatility sizes the position";
        EXPECT_DOUBLE_EQ(live->moving_average, bt->moving_average) << symbol;
        EXPECT_DOUBLE_EQ(live->std_deviation, bt->std_deviation) << symbol;
    }

    // z-scores and the signals derived from them.
    for (const auto& symbol : symbols_) {
        EXPECT_DOUBLE_EQ(bt_strat->get_z_score(symbol), live_strat->get_z_score(symbol))
            << symbol;
    }
    // NOT compared here: get_last_signals(). It is filled by BaseStrategy::on_signal(),
    // which only the three trend strategies call; MeanReversionStrategy::on_data() keeps
    // its signals in a local and never publishes them, so the map is empty on BOTH sides
    // and comparing it asserts 0 == 0. The previous version of this test did exactly
    // that. The signal is observed through the z-score and the target instead, which are
    // what it is computed from and what it produces.
    EXPECT_TRUE(bt_strat->get_last_signals().empty())
        << "if MeanReversion starts publishing signals, compare them here too";

    // The output the runners actually act on.
    const auto targets_bt = bt_strat->get_target_positions();
    const auto targets_live = live_strat->get_target_positions();
    ASSERT_EQ(targets_bt.size(), targets_live.size());
    for (const auto& [symbol, position] : targets_bt) {
        auto it = targets_live.find(symbol);
        ASSERT_NE(it, targets_live.end()) << "live missing target for " << symbol;
        EXPECT_DOUBLE_EQ(position.quantity.as_double(), it->second.quantity.as_double())
            << "target mismatch for " << symbol;
    }

    bt_strat->stop();
    live_strat->stop();
}

// The seeded book must survive the live staging: seeding is what lets generate_signal()
// take the exit branch and lets the stop-loss measure from a basis (F-B). A staging
// change that quietly dropped it would make every held position look flat again.
TEST_F(LiveBtSignalConsistencyTest, LiveStagingKeepsTheSeededBook) {
    auto mr = apps::build_mean_reversion_config(config_json());
    auto strat = make_strategy("LIVE_SEED_SURVIVES", mr);

    const auto book = held_book();
    for (const auto& symbol : symbols_) {
        feed_live_sequence(*strat, book, make_bars(symbol, kBars, 150.0, 42));
    }

    const auto positions = strat->get_positions();
    for (const auto& symbol : symbols_) {
        ASSERT_EQ(positions.count(symbol), 1u) << symbol << " lost its seeded holding";
        EXPECT_DOUBLE_EQ(positions.at(symbol).quantity.as_double(), 25.0);
        EXPECT_DOUBLE_EQ(positions.at(symbol).average_price.as_double(), 148.0)
            << "a rename-free staging pass must not restate the cost basis";
        EXPECT_DOUBLE_EQ(positions.at(symbol).realized_pnl.as_double(), 0.0)
            << "seeding zeroes realized: the column is a daily flow (E2-F19)";
    }

    strat->stop();
}

// The shared builder still has to produce one config from one JSON block -- the property
// the previous version of this file was really testing. Kept, stated as what it is.
TEST_F(LiveBtSignalConsistencyTest, SharedBuilderIsDeterministic) {
    const auto a = apps::build_mean_reversion_config(config_json());
    const auto b = apps::build_mean_reversion_config(config_json());

    EXPECT_EQ(a.lookback_period, b.lookback_period);
    EXPECT_DOUBLE_EQ(a.entry_threshold, b.entry_threshold);
    EXPECT_DOUBLE_EQ(a.exit_threshold, b.exit_threshold);
    EXPECT_DOUBLE_EQ(a.risk_target, b.risk_target);
    EXPECT_DOUBLE_EQ(a.position_size, b.position_size);
    EXPECT_EQ(a.vol_lookback, b.vol_lookback);
    EXPECT_EQ(a.use_stop_loss, b.use_stop_loss);
    EXPECT_DOUBLE_EQ(a.stop_loss_pct, b.stop_loss_pct);
    EXPECT_EQ(a.allow_fractional_shares, b.allow_fractional_shares);
}
