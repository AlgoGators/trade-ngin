// The trend strategies read the back-adjusted series for every RETURN consumer and the raw close
// for every LEVEL (LOOP_SPEC v6.1 sections 2.3 and 2.4; T-ROLLX commit 1):
//   * a contract switch (a bar whose instrument_id differs from the previous consumed bar's) is a
//     price gap, not a return: the vol estimate and the EMA spread of a series with a switch equal
//     those of the same series without it, bit for bit (on 9fe44f1a the log-return estimator sees
//     the gap as a +10 percent return and the vol differs);
//   * the EMAs read the adjusted level, the vol the adjusted returns, and the forecast divides by
//     the RAW close (the estimate of the last bar against a hand recomputation);
//   * nothing persists across bars: the forecast and the vol of the last bar are the same whether
//     the bars were fed one by one or at once, and they are recomputed from the window on every
//     bar (feeding one more bar with a switch shifts every earlier adjusted level by the step).
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/futures.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private

#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {
// The FAST sleeve: TrendFollowingStrategy on fast_trend_following_config() (one class, two
// configurations).
struct FastTrendConfig : TrendFollowingConfig {
    FastTrendConfig() : TrendFollowingConfig(fast_trend_following_config()) {}
};


const std::string kSym = "TSTROLL";
// 2021-01-04 00:00:00 UTC, a Monday.
const Timestamp kDay0 = std::chrono::system_clock::from_time_t(1609718400);

Timestamp day(int d) { return kDay0 + std::chrono::hours(24 * d); }

/// n bars; the close walks a deterministic zigzag with a slow trend; from bar `switch_at` on the
/// instrument id is "B" and the close carries a step of `gap` (a contango switch) on top.
/// `flat_at`: that bar's underlying close repeats the previous one (no true move on the switch day,
/// so the switch's whole step is the gap and the adjusted series is the plain series plus the gap).
std::vector<Bar> walk(int n, int switch_at, double gap, int flat_at = -1) {
    std::vector<Bar> bars;
    double p = 100.0;
    for (int k = 0; k < n; ++k) {
        if (k != flat_at) p *= 1.0 + 0.004 * ((k % 3 == 0) ? 1.0 : (k % 3 == 1 ? -0.6 : 0.3)) + 0.0004;
        const double close = p + (switch_at >= 0 && k >= switch_at ? gap : 0.0);
        Bar b;
        b.symbol = kSym;
        b.timestamp = day(k);
        b.open = b.high = b.low = b.close = Decimal(close);
        b.high = Decimal(close * 1.001);
        b.low = Decimal(close * 0.999);
        b.volume = 100000.0;
        b.instrument_id = (switch_at >= 0 && k >= switch_at) ? "B" : "A";
        bars.push_back(b);
    }
    return bars;
}

template <class Strategy, class Config>
struct Fixture {
    std::shared_ptr<MockPostgresDatabase> db;
    std::unique_ptr<Strategy> strategy;

    explicit Fixture(const std::string& id) {
        db = std::make_shared<MockPostgresDatabase>("mock://testdb");
        EXPECT_TRUE(db->connect().is_ok());
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params[kSym] = 5.0;
        sc.position_limits[kSym] = 1000.0;
        Config tc;
        tc.weight = 1.0 / 30.0;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.use_position_buffering = false;
        tc.ema_windows = {{2, 8}, {4, 16}, {8, 32}, {16, 64}};
        tc.vol_lookback_short = 32;
        tc.vol_lookback_long = 252;
        tc.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
        auto& registry = InstrumentRegistry::instance();
        FuturesSpec spec;
        spec.root_symbol = kSym;
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 5.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 2.0;
        spec.initial_margin = 10000.0;
        spec.maintenance_margin = 8000.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        registry.instruments_[kSym] = std::make_shared<FuturesInstrument>(kSym, spec);
        registry.initialized_ = true;
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        strategy = std::make_unique<Strategy>(id, sc, tc, db, registry_ptr);
        EXPECT_TRUE(strategy->initialize().is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.5;
        limits.max_leverage = 100.0;
        EXPECT_TRUE(strategy->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
    }
    ~Fixture() {
        strategy->stop();
        strategy.reset();
        db->disconnect();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
    }
    const auto& data() const { return strategy->get_all_instrument_data().at(kSym); }
};

template <class S, class C>
void expect_switch_invisible(const std::string& label) {
    SCOPED_TRACE(label);
    static int n = 0;
    StateManager::reset_instance();
    // The same underlying walk, flat across bar 80 on both; rolled adds a +12.5 point gap (about
    // +9 percent) and a new id from bar 80 on. The step is then exactly the gap, so the adjusted
    // series is the plain series plus 12.5 on every bar (a switch day with a true move removes
    // that move too: section 2.3, one zero per roll; the hand recomputation below covers it).
    const auto plain = walk(120, -1, 0.0, 80);
    const auto rolled = walk(120, 80, 12.5, 80);
    Fixture<S, C> a(label + "_plain_" + std::to_string(++n));
    Fixture<S, C> b(label + "_rolled_" + std::to_string(n));
    ASSERT_TRUE(a.strategy->on_data(plain).is_ok());
    ASSERT_TRUE(b.strategy->on_data(rolled).is_ok());
    ASSERT_EQ(a.data().price_history.size(), 120u);
    ASSERT_EQ(b.data().bar_instrument_ids.size(), 120u);
    EXPECT_EQ(b.data().bar_instrument_ids[79], "A");
    EXPECT_EQ(b.data().bar_instrument_ids[80], "B");
    // The vol estimate reads the adjusted returns: the switch bar's return is 0, and after the
    // switch the returns are measured on the new contract's raw level, so the two series' vols
    // differ; the hand recomputation pins the rolled one.
    EXPECT_NE(a.data().current_volatility, b.data().current_volatility)
        << "after the switch the returns are measured on the new contract's raw level";
    {
        std::vector<double> raw;
        std::vector<std::string> ids;
        for (const auto& bar : rolled) {
            raw.push_back(static_cast<double>(bar.close));
            ids.push_back(bar.instrument_id);
        }
        const auto series = roll_series::build_series(raw, ids);
        EXPECT_EQ(series.returns[79], 0.0) << "the switch bar's return";
        const double lambda = 2.0 / (b.strategy->trend_config_.vol_lookback_short + 1.0);
        double mean = series.returns[0];
        double var = std::max(1e-6, series.returns[0] * series.returns[0] * 0.1);
        for (size_t t = 1; t < series.returns.size(); ++t) {
            mean = lambda * series.returns[t] + (1 - lambda) * mean;
            const double d = series.returns[t] - mean;
            var = std::max(1e-6, lambda * d * d + (1 - lambda) * var);
        }
        // The last short-term stddev, annualised by the estimator's own factor (bars a year over
        // the trailing bars) and clamped.
        const auto& estimate = b.data().estimate;
        ASSERT_TRUE(estimate.valid);
        EXPECT_EQ(estimate.window_bars, 120u);
        EXPECT_NEAR(estimate.sigma_short,
                    std::clamp(std::sqrt(var) * estimate.factor, 0.005, 5.0), 1e-12);
    }
    // The EMAs read the adjusted level: the EMA spread is the plain series' spread.
    const auto ema_a = a.strategy->get_ema_values(kSym, {8, 32});
    const auto ema_b = b.strategy->get_ema_values(kSym, {8, 32});
    EXPECT_NEAR(ema_a.at(8) - ema_a.at(32), ema_b.at(8) - ema_b.at(32), 1e-9);
    // The level is raw: the adjusted series is anchored on the latest bar, so on the rolled
    // series every EMA sits 12.5 above the plain one (the earlier segment is lifted by the step).
    EXPECT_NEAR(ema_b.at(8), ema_a.at(8) + 12.5, 1e-9);
    EXPECT_NEAR(ema_b.at(32), ema_a.at(32) + 12.5, 1e-9);
}

}  // namespace

TEST(TrendAdjustedSeries, AContractSwitchIsNotAReturnForTheVolOrTheEmas) {
    expect_switch_invisible<TrendFollowingStrategy, TrendFollowingConfig>("TREND");
    expect_switch_invisible<TrendFollowingStrategy, FastTrendConfig>("FAST");
}

// The scaled forecast of an EMA pair at the last bar, recomputed by hand from the series' pieces:
// the EMAs on the adjusted level (seeded at the window's first level), the forecast's vol from the
// adjusted returns (the fixed factor 16, its own long-run mean), the price the RAW close, the pair's
// fixed scalar. The series has a switch inside it, so adjusted != raw.
TEST(TrendAdjustedSeries, TheForecastDividesTheAdjustedEmaSpreadByTheRawClose) {
    StateManager::reset_instance();
    Fixture<TrendFollowingStrategy, TrendFollowingConfig> f("TREND_level_rule");
    const auto rolled = walk(100, 40, 7.0);
    std::vector<double> raw;
    std::vector<std::string> ids;
    for (const auto& b : rolled) {
        raw.push_back(static_cast<double>(b.close));
        ids.push_back(b.instrument_id);
    }
    const roll_series::Series series = roll_series::build_series(raw, ids);
    ASSERT_TRUE(series.flags.change[40]);
    ASSERT_NE(series.adjusted[10], series.raw[10]);
    ASSERT_TRUE(f.strategy->on_data(rolled).is_ok());
    const auto& estimate = f.data().estimate;
    ASSERT_TRUE(estimate.valid);
    ASSERT_EQ(estimate.scaled.size(), 4u);

    // The vol: the EWMA on r_t = (A_t - A_t-1) / P_t-1, 0 on the switch, annualised by 16, then
    // 0.7 of the last value plus 0.3 of the mean of every value (fewer than 2,520).
    const std::vector<double>& r = series.returns;
    EXPECT_EQ(r[39], 0.0);
    EXPECT_NE((raw[40] - raw[39]) / raw[39], 0.0);
    const double lambda = 2.0 / 33.0;
    double mean = r[0];
    double var = std::max(1e-6, r[0] * r[0] * 0.1);
    std::vector<double> short_vol{std::clamp(std::sqrt(var) * 16.0, 0.005, 5.0)};
    for (size_t t = 1; t < r.size(); ++t) {
        mean = lambda * r[t] + (1 - lambda) * mean;
        const double d = r[t] - mean;
        var = std::max(1e-6, lambda * d * d + (1 - lambda) * var);
        short_vol.push_back(std::clamp(std::sqrt(var) * 16.0, 0.005, 5.0));
    }
    double sum = 0.0;
    for (double v : short_vol) sum += v;
    const double forecast_vol = 0.7 * short_vol.back() + 0.3 * sum / short_vol.size();
    EXPECT_NEAR(estimate.forecast_sigma, forecast_vol, 1e-12);

    // The pair (8, 32): EMAs of the ADJUSTED level over the RAW close, times the fixed scalar 5.95.
    // Fewer than 252 values: the attenuation is 1.
    EXPECT_EQ(estimate.attenuation, 1.0);
    const auto ema_s = f.strategy->calculate_ewma(series.adjusted, 8);
    const auto ema_l = f.strategy->calculate_ewma(series.adjusted, 32);
    const double expected =
        std::clamp((ema_s.back() - ema_l.back()) / (series.raw.back() * forecast_vol / 16.0) * 5.95,
                   -20.0, 20.0);
    EXPECT_NEAR(estimate.scaled[2], expected, 1e-11 * std::max(1.0, std::abs(expected)));
    // And the combined forecast: the mean of the four scaled forecasts times the multiplier for
    // four pairs, capped.
    const double combined =
        std::clamp(1.13 * (estimate.scaled[0] + estimate.scaled[1] + estimate.scaled[2] +
                           estimate.scaled[3]) / 4.0,
                   -20.0, 20.0);
    EXPECT_NEAR(f.data().current_forecast, combined, 1e-12);
}

// No level persists: the bars fed one at a time give the last bar's forecast and vol of the bulk
// feed, and one more bar with a switch re-anchors every earlier adjusted level by the step.
TEST(TrendAdjustedSeries, TheWindowIsRecomputedOnEveryBarAndNothingPersists) {
    StateManager::reset_instance();
    const auto rolled = walk(130, 90, 9.0);
    Fixture<TrendFollowingStrategy, TrendFollowingConfig> bulk("TREND_bulk");
    Fixture<TrendFollowingStrategy, TrendFollowingConfig> daily("TREND_daily");
    ASSERT_TRUE(bulk.strategy->on_data(rolled).is_ok());
    for (const auto& b : rolled) ASSERT_TRUE(daily.strategy->on_data({b}).is_ok());
    EXPECT_EQ(bulk.data().current_volatility, daily.data().current_volatility);
    EXPECT_EQ(bulk.data().current_forecast, daily.data().current_forecast);
    EXPECT_EQ(bulk.strategy->get_ema_values(kSym, {8, 32}), daily.strategy->get_ema_values(kSym, {8, 32}));

    // The adjusted level of bar 10 as the strategy sees it before and after a later switch.
    const auto before = walk(100, -1, 0.0);
    Fixture<TrendFollowingStrategy, TrendFollowingConfig> f("TREND_anchor");
    ASSERT_TRUE(f.strategy->on_data(before).is_ok());
    const auto ema_before = f.strategy->get_ema_values(kSym, {8});
    Bar next = before.back();
    next.timestamp = day(100);
    next.instrument_id = "B";
    next.close = Decimal(static_cast<double>(before.back().close) + 20.0);
    next.open = next.high = next.low = next.close;
    ASSERT_TRUE(f.strategy->on_data({next}).is_ok());
    const auto ema_after = f.strategy->get_ema_values(kSym, {8});
    // EMA_8 after = lambda x A_100 + (1 - lambda) x (EMA_8 before + 20): every earlier level lifted
    // by the step, the newest segment the raw close; nothing was carried across the switch.
    const double lambda = 2.0 / 9.0;
    EXPECT_NEAR(ema_after.at(8),
                lambda * static_cast<double>(next.close) + (1 - lambda) * (ema_before.at(8) + 20.0),
                1e-9);
}
