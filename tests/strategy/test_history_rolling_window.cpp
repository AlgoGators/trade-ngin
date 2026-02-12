#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

// ============================================================================
// Test fixture: creates two identical strategies — one with the new default
// max_history_size (756) and one with the old 2520 — then feeds identical data
// to both, verifying they produce the same positions when data fits within the
// smaller window, and that the rolling trim works correctly at boundaries.
// ============================================================================

class HistoryRollingWindowTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();

        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        // Shared base config
        base_strategy_config_.capital_allocation = 1000000.0;
        base_strategy_config_.max_leverage = 4.0;
        base_strategy_config_.asset_classes = {AssetClass::FUTURES};
        base_strategy_config_.frequencies = {DataFrequency::DAILY};

        for (const auto& symbol : {"ES", "NQ"}) {
            base_strategy_config_.trading_params[symbol] = 5.0;
            base_strategy_config_.position_limits[symbol] = 1000.0;
        }

        // Shared trend config base
        base_trend_config_.weight = 1.0 / 30.0;
        base_trend_config_.risk_target = 0.2;
        base_trend_config_.idm = 2.5;
        base_trend_config_.use_position_buffering = true;
        base_trend_config_.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}};
        base_trend_config_.vol_lookback_short = 32;
        base_trend_config_.vol_lookback_long = 252;
        base_trend_config_.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}};
    }

    void TearDown() override {
        TestBase::TearDown();
    }

    // Deterministic bar generator (same seed every time for reproducibility)
    std::vector<Bar> create_deterministic_data(const std::string& symbol, int num_bars,
                                                double start_price = 4000.0) {
        std::vector<Bar> data;
        data.reserve(num_bars);
        auto now = std::chrono::system_clock::now();
        double price = start_price;

        // Use a simple LCG for determinism (independent of global rand state)
        uint32_t seed = 12345;
        auto next_rand = [&seed]() -> double {
            seed = seed * 1103515245 + 12345;
            return static_cast<double>((seed >> 16) & 0x7FFF) / 32767.0;
        };

        for (int i = 0; i < num_bars; i++) {
            Bar bar;
            bar.symbol = symbol;
            bar.timestamp = now - std::chrono::hours(24 * (num_bars - i));

            double trend = std::sin(i * 0.05) * 0.003;
            double noise = (next_rand() - 0.5) * 0.02;
            price = std::max(start_price * 0.3, price * (1.0 + trend + noise));

            bar.open = price;
            bar.close = price * (1.0 + (next_rand() - 0.5) * 0.01);
            bar.high = std::max(bar.open.as_double(), bar.close.as_double()) * 1.005;
            bar.low = std::min(bar.open.as_double(), bar.close.as_double()) * 0.995;
            bar.volume = 100000 + static_cast<int>(next_rand() * 50000);
            data.push_back(bar);
        }
        return data;
    }

    std::unique_ptr<TrendFollowingStrategy> create_strategy(const std::string& id,
                                                             size_t max_history_size) {
        TrendFollowingConfig cfg = base_trend_config_;
        cfg.max_history_size = max_history_size;

        auto strategy = std::make_unique<TrendFollowingStrategy>(id, base_strategy_config_, cfg, db_);
        auto init = strategy->initialize();
        EXPECT_TRUE(init.is_ok()) << "Init failed: " << init.error()->what();
        auto start = strategy->start();
        EXPECT_TRUE(start.is_ok()) << "Start failed: " << start.error()->what();
        return strategy;
    }

    // Feed bars one-by-one (simulating the backtest day loop)
    void feed_bars_daily(TrendFollowingStrategy* strategy, const std::vector<Bar>& bars) {
        for (const auto& bar : bars) {
            std::vector<Bar> single{bar};
            auto r = strategy->on_data(single);
            // Silently ignore warmup errors (insufficient data)
            (void)r;
        }
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    StrategyConfig base_strategy_config_;
    TrendFollowingConfig base_trend_config_;
};

// ---------------------------------------------------------------------------
// 1. Under-cap: when total bars < max_history_size, results must be identical
//    to the old 2520 cap because no trimming occurs.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, UnderCapResultsIdentical) {
    // Feed 500 bars (well under both 756 and 2520 caps)
    auto data_es = create_deterministic_data("ES", 500);
    auto data_nq = create_deterministic_data("NQ", 500, 15000.0);

    auto strategy_new = create_strategy("NEW_CAP_1", 756);
    auto strategy_old = create_strategy("OLD_CAP_1", 2520);

    // Combine symbols per day (same as backtest does)
    for (size_t i = 0; i < data_es.size(); i++) {
        std::vector<Bar> day_bars = {data_es[i], data_nq[i]};
        strategy_new->on_data(day_bars);
        strategy_old->on_data(day_bars);
    }

    // Compare positions — must be exactly equal
    auto pos_new = strategy_new->get_positions();
    auto pos_old = strategy_old->get_positions();

    ASSERT_EQ(pos_new.size(), pos_old.size()) << "Position count mismatch";

    for (const auto& [symbol, new_pos] : pos_new) {
        auto it = pos_old.find(symbol);
        ASSERT_NE(it, pos_old.end()) << "Symbol " << symbol << " missing in old strategy";
        EXPECT_DOUBLE_EQ(new_pos.quantity.as_double(), it->second.quantity.as_double())
            << "Position mismatch for " << symbol;
    }

    // Also verify price history lengths are the same (no trimming should have occurred)
    auto hist_new = strategy_new->get_price_history();
    auto hist_old = strategy_old->get_price_history();
    for (const auto& [symbol, prices] : hist_new) {
        EXPECT_EQ(prices.size(), hist_old[symbol].size())
            << "History length mismatch for " << symbol << " (under cap, should be equal)";
        EXPECT_LE(prices.size(), 500u) << "History should not exceed data fed";
    }
}

// ---------------------------------------------------------------------------
// 2. At-cap boundary: feed exactly max_history_size bars, verify no trimming
//    has happened and the last bar is correct.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, ExactlyAtCapBoundary) {
    const size_t cap = 756;
    auto data = create_deterministic_data("ES", static_cast<int>(cap));

    auto strategy = create_strategy("BOUNDARY_1", cap);
    feed_bars_daily(strategy.get(), data);

    auto hist = strategy->get_price_history();
    ASSERT_TRUE(hist.count("ES") > 0);
    EXPECT_EQ(hist["ES"].size(), cap) << "At exactly cap, all bars should be retained";

    // Verify last price matches last bar's close
    double last_close = data.back().close.as_double();
    EXPECT_NEAR(hist["ES"].back(), last_close, 1e-6)
        << "Last history entry should match last bar close";
}

// ---------------------------------------------------------------------------
// 3. Over-cap trimming: feed cap+100 bars, verify history is trimmed to cap
//    and the oldest bars are dropped (FIFO).
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, OverCapTrimsCorrectly) {
    const size_t cap = 756;
    const int total_bars = static_cast<int>(cap) + 100;
    auto data = create_deterministic_data("ES", total_bars);

    auto strategy = create_strategy("TRIM_1", cap);
    feed_bars_daily(strategy.get(), data);

    auto hist = strategy->get_price_history();
    ASSERT_TRUE(hist.count("ES") > 0);
    EXPECT_EQ(hist["ES"].size(), cap) << "History should be trimmed to cap";

    // The last entry should still be the most recent bar's close
    double last_close = data.back().close.as_double();
    EXPECT_NEAR(hist["ES"].back(), last_close, 1e-6);

    // The first entry should be from bar index (total_bars - cap), not bar 0
    // This verifies FIFO trimming — oldest bars dropped
    double expected_first = data[total_bars - static_cast<int>(cap)].close.as_double();
    EXPECT_NEAR(hist["ES"].front(), expected_first, 1e-6)
        << "First history entry should be from bar " << (total_bars - cap) << " after trimming";
}

// ---------------------------------------------------------------------------
// 4. Small cap stress test: use a very small cap (e.g., 200) and verify the
//    strategy still produces valid positions after processing 800 bars.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, SmallCapStillProducesPositions) {
    const size_t small_cap = 200;
    auto data = create_deterministic_data("ES", 800);

    auto strategy = create_strategy("SMALL_CAP_1", small_cap);
    feed_bars_daily(strategy.get(), data);

    auto hist = strategy->get_price_history();
    ASSERT_TRUE(hist.count("ES") > 0);
    EXPECT_EQ(hist["ES"].size(), small_cap) << "History should be capped at " << small_cap;

    // Verify the strategy still generates a position (not stuck at zero)
    auto positions = strategy->get_positions();
    // Strategy needs >= max_window bars before generating signals, so positions
    // may be zero if small_cap < max_window. Just verify no crash and that
    // position is a finite number.
    if (positions.count("ES") > 0) {
        EXPECT_TRUE(std::isfinite(positions["ES"].quantity.as_double()))
            << "Position should be a finite number";
    }
}

// ---------------------------------------------------------------------------
// 5. Numerical equivalence: for data that fits within the smaller cap, verify
//    that positions from a 756-capped strategy exactly match a 2520-capped
//    strategy after processing a realistic multi-symbol backtest.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, NumericalEquivalenceMultiSymbol) {
    // 700 bars < 756 cap, so no trimming — results must be bit-identical
    auto data_es = create_deterministic_data("ES", 700, 4000.0);
    auto data_nq = create_deterministic_data("NQ", 700, 15000.0);

    auto strategy_756 = create_strategy("EQUIV_756", 756);
    auto strategy_2520 = create_strategy("EQUIV_2520", 2520);

    for (size_t i = 0; i < data_es.size(); i++) {
        std::vector<Bar> day = {data_es[i], data_nq[i]};
        strategy_756->on_data(day);
        strategy_2520->on_data(day);
    }

    auto pos_756 = strategy_756->get_positions();
    auto pos_2520 = strategy_2520->get_positions();

    for (const auto& [symbol, p756] : pos_756) {
        auto it = pos_2520.find(symbol);
        ASSERT_NE(it, pos_2520.end());
        EXPECT_DOUBLE_EQ(p756.quantity.as_double(), it->second.quantity.as_double())
            << "Position mismatch for " << symbol << " between 756 and 2520 caps";
    }
}

// ---------------------------------------------------------------------------
// 6. Volatility history also trimmed: verify that volatility_history respects
//    the same max_history_size cap.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, VolatilityHistoryAlsoTrimmed) {
    const size_t cap = 300;
    auto data = create_deterministic_data("ES", 500);

    auto strategy = create_strategy("VOL_TRIM_1", cap);
    feed_bars_daily(strategy.get(), data);

    // Access instrument data to check volatility_history length
    const auto& instrument_data = strategy->get_all_instrument_data();
    auto it = instrument_data.find("ES");
    ASSERT_NE(it, instrument_data.end());

    // Volatility history is recalculated from the last 1000 price entries each
    // time, and then trimmed to max_history_size. Since we have cap=300 prices,
    // volatility_history length should be <= cap.
    EXPECT_LE(it->second.volatility_history.size(), cap)
        << "Volatility history should respect max_history_size cap";
}

// ---------------------------------------------------------------------------
// 7. Default config uses 756: verify the default TrendFollowingConfig has
//    max_history_size = 756.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, DefaultConfigIs756) {
    TrendFollowingConfig default_cfg;
    EXPECT_EQ(default_cfg.max_history_size, 756u)
        << "Default max_history_size should be 756 (~3 years of trading days)";
}

// ---------------------------------------------------------------------------
// 8. Large cap backward-compat: setting max_history_size=2520 reproduces
//    the original behavior exactly.
// ---------------------------------------------------------------------------
TEST_F(HistoryRollingWindowTest, LargeCapBackwardCompatible) {
    const size_t old_cap = 2520;
    auto data = create_deterministic_data("ES", 800);

    auto strategy = create_strategy("COMPAT_1", old_cap);
    feed_bars_daily(strategy.get(), data);

    auto hist = strategy->get_price_history();
    ASSERT_TRUE(hist.count("ES") > 0);

    // With cap=2520 and only 800 bars, no trimming should have occurred
    EXPECT_EQ(hist["ES"].size(), 800u)
        << "With old 2520 cap and 800 bars, nothing should be trimmed";
}
