#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/strategy/regime_detector.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

RegimeDetectorConfig small_config(int lookback = 20) {
    RegimeDetectorConfig c;
    c.lookback_period = lookback;
    c.confidence_threshold = 0.75;
    c.min_regime_duration = 5;
    c.use_machine_learning = false;
    c.var_ratio_threshold = 2.0;
    c.correlation_threshold = 0.7;
    c.volatility_threshold = 0.3;
    return c;
}

Bar make_bar(const std::string& symbol, double close,
             std::chrono::system_clock::time_point ts, double volume = 1000.0) {
    Bar bar;
    bar.symbol = symbol;
    bar.timestamp = ts;
    bar.open = Decimal(close);
    bar.high = Decimal(close);
    bar.low = Decimal(close);
    bar.close = Decimal(close);
    bar.volume = volume;
    return bar;
}

// Build a strict uptrend price series that drives trend_strength near +1
std::vector<Bar> uptrend_bars(const std::string& symbol, int n,
                              std::chrono::system_clock::time_point t0,
                              double start = 100.0, double step = 1.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        bars.push_back(
            make_bar(symbol, start + i * step, t0 + std::chrono::hours(24 * i)));
    }
    return bars;
}

}  // namespace

class RegimeDetectorTest : public TestBase {};

TEST_F(RegimeDetectorTest, ConstructorRegistersWithStateManager) {
    // After construction, StateManager has a registered REGIME_DETECTOR component.
    RegimeDetector det(small_config());
    auto info = StateManager::instance().get_state("REGIME_DETECTOR");
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().state, ComponentState::INITIALIZED);
}

TEST_F(RegimeDetectorTest, InitializeTransitionsStateToRunning) {
    RegimeDetector det(small_config());
    ASSERT_TRUE(det.initialize().is_ok());
    auto info = StateManager::instance().get_state("REGIME_DETECTOR");
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().state, ComponentState::RUNNING);
}

TEST_F(RegimeDetectorTest, GettersReturnErrorForUnknownSymbol) {
    RegimeDetector det(small_config());
    ASSERT_TRUE(det.initialize().is_ok());
    EXPECT_TRUE(det.get_regime("UNKNOWN").is_error());
    EXPECT_TRUE(det.get_features("UNKNOWN").is_error());
    EXPECT_TRUE(det.has_regime_changed("UNKNOWN").is_error());
    EXPECT_TRUE(det.get_change_probability("UNKNOWN").is_error());
    EXPECT_TRUE(det.get_regime_history("UNKNOWN").is_error());
}

TEST_F(RegimeDetectorTest, UpdateWithEmptyDataIsNoOp) {
    RegimeDetector det(small_config());
    ASSERT_TRUE(det.update({}).is_ok());
    EXPECT_TRUE(det.get_regime("ES").is_error());
}

TEST_F(RegimeDetectorTest, InsufficientHistoryDoesNotPopulateRegime) {
    RegimeDetector det(small_config(20));
    auto t0 = std::chrono::system_clock::now();
    // Feed only 10 bars when lookback_period = 20 → no regime detection.
    auto bars = uptrend_bars("ES", 10, t0);
    ASSERT_TRUE(det.update(bars).is_ok());
    EXPECT_TRUE(det.get_regime("ES").is_error());
}

TEST_F(RegimeDetectorTest, SufficientHistoryPopulatesRegime) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    auto bars = uptrend_bars("ES", 30, t0);  // 30 bars > lookback 10
    ASSERT_TRUE(det.update(bars).is_ok());
    EXPECT_TRUE(det.get_regime("ES").is_ok());
    EXPECT_TRUE(det.get_features("ES").is_ok());
    EXPECT_TRUE(det.has_regime_changed("ES").is_ok());
    EXPECT_TRUE(det.get_change_probability("ES").is_ok());
}

TEST_F(RegimeDetectorTest, UptrendSeriesProducesPositiveTrendStrength) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    ASSERT_TRUE(det.update(uptrend_bars("ES", 30, t0)).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    EXPECT_GT(features.value().trend_strength, 0.5);  // Strong positive trend
}

TEST_F(RegimeDetectorTest, DowntrendSeriesProducesNegativeTrendStrength) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    auto bars = uptrend_bars("ES", 30, t0, /*start=*/200.0, /*step=*/-1.0);
    ASSERT_TRUE(det.update(bars).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    EXPECT_LT(features.value().trend_strength, -0.5);
}

TEST_F(RegimeDetectorTest, FlatSeriesProducesNeutralTrendStrength) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        bars.push_back(make_bar("ES", 100.0, t0 + std::chrono::hours(24 * i)));
    }
    ASSERT_TRUE(det.update(bars).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    EXPECT_DOUBLE_EQ(features.value().trend_strength, 0.0);
}

TEST_F(RegimeDetectorTest, LookbackWindowSlidesWhenNewBarsArrive) {
    // After 25 bars with lookback=10, only the last 10 prices contribute.
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();

    std::vector<Bar> bars;
    for (int i = 0; i < 15; ++i) {
        bars.push_back(make_bar("ES", 100.0, t0 + std::chrono::hours(24 * i)));
    }
    for (int i = 15; i < 25; ++i) {
        bars.push_back(make_bar("ES", 100.0 + (i - 14) * 1.0,
                                 t0 + std::chrono::hours(24 * i)));
    }
    ASSERT_TRUE(det.update(bars).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    // Recent window is monotonically increasing, so trend_strength is positive.
    EXPECT_GT(features.value().trend_strength, 0.0);
}

TEST_F(RegimeDetectorTest, MultipleSymbolsTrackedIndependently) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        auto ts = t0 + std::chrono::hours(24 * i);
        bars.push_back(make_bar("ES", 100.0 + i * 1.0, ts));     // uptrend
        bars.push_back(make_bar("NQ", 200.0 - i * 0.5, ts));    // downtrend
    }
    ASSERT_TRUE(det.update(bars).is_ok());
    auto es = det.get_features("ES");
    auto nq = det.get_features("NQ");
    ASSERT_TRUE(es.is_ok());
    ASSERT_TRUE(nq.is_ok());
    EXPECT_GT(es.value().trend_strength, 0.5);
    EXPECT_LT(nq.value().trend_strength, -0.5);
}

TEST_F(RegimeDetectorTest, VolatilityIsNonNegative) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    // Create a noisy series.
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        double price = 100.0 + (i % 2 == 0 ? 5.0 : -5.0);
        bars.push_back(make_bar("ES", price, t0 + std::chrono::hours(24 * i)));
    }
    ASSERT_TRUE(det.update(bars).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    EXPECT_GE(features.value().volatility, 0.0);
}

TEST_F(RegimeDetectorTest, HurstExponentForVaryingSeriesIsFinite) {
    // Use a varying series so the rolling-window stds aren't all zero.
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        double price = 100.0 + std::sin(i * 0.5) * 5.0;
        bars.push_back(make_bar("ES", price, t0 + std::chrono::hours(24 * i)));
    }
    ASSERT_TRUE(det.update(bars).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    EXPECT_TRUE(std::isfinite(features.value().hurst_exponent));
}

TEST_F(RegimeDetectorTest, RegimeProbabilityIsBoundedAndFinite) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    ASSERT_TRUE(det.update(uptrend_bars("ES", 30, t0)).is_ok());
    auto p = det.get_change_probability("ES");
    ASSERT_TRUE(p.is_ok());
    EXPECT_TRUE(std::isfinite(p.value()));
    EXPECT_GE(p.value(), 0.0);
}

TEST_F(RegimeDetectorTest, GetRegimeHistoryEmptyUntilRegimeChange) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    ASSERT_TRUE(det.update(uptrend_bars("ES", 30, t0)).is_ok());
    // First-time entry doesn't go into history (validate_regime_change returns
    // true only when current_regimes_ already has the symbol).
    auto hist = det.get_regime_history("ES");
    EXPECT_TRUE(hist.is_error());
}

TEST_F(RegimeDetectorTest, RegimeChangedFlagComparesDurationToMinRegimeDuration) {
    auto cfg = small_config(10);
    cfg.min_regime_duration = 1000;  // huge so duration always < threshold
    RegimeDetector det(cfg);
    auto t0 = std::chrono::system_clock::now();
    ASSERT_TRUE(det.update(uptrend_bars("ES", 30, t0)).is_ok());
    auto changed = det.has_regime_changed("ES");
    ASSERT_TRUE(changed.is_ok());
    EXPECT_TRUE(changed.value());
}

TEST_F(RegimeDetectorTest, RegimeChangedFlagFalseWhenDurationExceedsMinimum) {
    auto cfg = small_config(10);
    cfg.min_regime_duration = 1;  // tiny so duration ramps past it quickly
    RegimeDetector det(cfg);
    auto t0 = std::chrono::system_clock::now();
    ASSERT_TRUE(det.update(uptrend_bars("ES", 30, t0)).is_ok());
    auto changed = det.has_regime_changed("ES");
    ASSERT_TRUE(changed.is_ok());
    EXPECT_FALSE(changed.value());
}

TEST_F(RegimeDetectorTest, UpdateProcessesIncrementalBatches) {
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    // Feed in two separate batches; second batch should not destabilize state.
    auto first = uptrend_bars("ES", 15, t0);
    std::vector<Bar> second;
    for (int i = 15; i < 30; ++i) {
        second.push_back(make_bar("ES", 100.0 + i, t0 + std::chrono::hours(24 * i)));
    }
    ASSERT_TRUE(det.update(first).is_ok());
    ASSERT_TRUE(det.update(second).is_ok());
    EXPECT_TRUE(det.get_regime("ES").is_ok());
}

TEST_F(RegimeDetectorTest, MeanReversionConfigThresholdRespected) {
    auto cfg = small_config(10);
    cfg.var_ratio_threshold = 100.0;  // very forgiving threshold
    RegimeDetector det(cfg);
    auto t0 = std::chrono::system_clock::now();
    // Mean-reverting (oscillating) prices.
    std::vector<Bar> bars;
    for (int i = 0; i < 30; ++i) {
        double price = 100.0 + (i % 2 == 0 ? 1.0 : -1.0);
        bars.push_back(make_bar("ES", price, t0 + std::chrono::hours(24 * i)));
    }
    ASSERT_TRUE(det.update(bars).is_ok());
    auto features = det.get_features("ES");
    ASSERT_TRUE(features.is_ok());
    EXPECT_GE(features.value().mean_reversion_strength, 0.0);
    EXPECT_LE(features.value().mean_reversion_strength, 1.0);
}

TEST_F(RegimeDetectorTest, ConcurrentSymbolUpdatesPreserveLockBehavior) {
    // Single-threaded smoke check that mutex usage doesn't deadlock when calls
    // alternate between mutating update() and read-only getters.
    RegimeDetector det(small_config(10));
    auto t0 = std::chrono::system_clock::now();
    auto bars = uptrend_bars("ES", 30, t0);
    ASSERT_TRUE(det.update(bars).is_ok());
    EXPECT_TRUE(det.get_features("ES").is_ok());
    ASSERT_TRUE(det.update({make_bar("ES", 200.0, t0 + std::chrono::hours(24 * 30))}).is_ok());
    EXPECT_TRUE(det.get_regime("ES").is_ok());
}
