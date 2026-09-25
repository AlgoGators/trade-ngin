// The trend strategies' vol estimate is annualised by the bars a year the series actually has
// (T-VOL C1). The per-bar EWMA stddev used to be multiplied by a fixed 16 (sqrt 256) on every
// bar, including the Sunday session bars most futures print: a six-bar week then spreads the
// same weekly variance over six bars and 16 understates the annual vol by about 8.6 percent.
//
// Synthetic series: log returns alternate +r / -r, so the EWMA per-bar stddev settles at
// r * (N - 1) / N for an EWMA span N (the EWMA mean oscillates at +/- r / N around zero). A
// week of b bars then carries a weekly variance of b * sigma^2, and the known annual vol is
// sqrt(b * sigma^2 * 365.25 / 7). The strategies must return it within 0.5 percent (the
// estimator's start-up transient is about 0.15 percent here).
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/instruments/futures.hpp"

// Expose private members so the singleton registry can be populated without a database
// (the pattern of test_trend_following.cpp)
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

#include "trade_ngin/strategy/trend_following.hpp"
#include "trade_ngin/strategy/trend_following_fast.hpp"
#include "trade_ngin/strategy/trend_following_slow.hpp"
#include "trade_ngin/strategy/vol_annualisation.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::string kSym = "TSTVOL";
constexpr double kR = 0.01;          // per-bar log return magnitude
constexpr double kTolerance = 0.005;  // relative

// 2021-01-03 00:00:00 UTC, a Sunday.
const Timestamp kSunday0 = std::chrono::system_clock::from_time_t(1609632000);

Timestamp day(int d) { return kSunday0 + std::chrono::hours(24 * d); }

// Six-bar week: Sunday session row plus Monday to Friday. Five-bar week: Monday to Friday.
std::vector<Timestamp> calendar(int n, int bars_per_week, int first_day = 0) {
    static const int six[] = {0, 1, 2, 3, 4, 5};
    static const int five[] = {1, 2, 3, 4, 5};
    const int* offsets = bars_per_week == 6 ? six : five;
    std::vector<Timestamp> out;
    out.reserve(n);
    for (int k = 0; k < n; ++k) {
        out.push_back(day(first_day + 7 * (k / bars_per_week) + offsets[k % bars_per_week]));
    }
    return out;
}

std::vector<Bar> alternating_bars(const std::vector<Timestamp>& ts) {
    std::vector<Bar> bars;
    bars.reserve(ts.size());
    for (size_t k = 0; k < ts.size(); ++k) {
        const double p = 100.0 * std::exp(kR * static_cast<double>(k % 2));
        Bar b;
        b.symbol = kSym;
        b.timestamp = ts[k];
        b.open = p;
        b.close = p;
        b.high = p * 1.001;
        b.low = p * 0.999;
        b.volume = 1000.0;
        bars.push_back(b);
    }
    return bars;
}

double known_annual_vol(int vol_span, int bars_per_week) {
    const double sigma = kR * (vol_span - 1) / vol_span;
    return std::sqrt(bars_per_week * sigma * sigma * kDaysPerYear / 7.0);
}

template <class S>
struct Traits;
template <>
struct Traits<TrendFollowingStrategy> {
    using Config = TrendFollowingConfig;
    static constexpr int kVolSpan = 32;
};
template <>
struct Traits<TrendFollowingFastStrategy> {
    using Config = TrendFollowingFastConfig;
    static constexpr int kVolSpan = 16;
};
template <>
struct Traits<TrendFollowingSlowStrategy> {
    using Config = TrendFollowingSlowConfig;
    static constexpr int kVolSpan = 64;
};

}  // namespace

template <class S>
class VolAnnualisationTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        StateManager::reset_instance();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());

        auto& registry = InstrumentRegistry::instance();
        FuturesSpec spec;
        spec.root_symbol = kSym;
        spec.exchange = "CME";
        spec.currency = "USD";
        spec.multiplier = 50.0;
        spec.tick_size = 0.25;
        spec.commission_per_contract = 2.0;
        spec.initial_margin = 10000.0;
        spec.maintenance_margin = 8000.0;
        spec.weight = 1.0;
        spec.trading_hours = "09:30-16:00";
        registry.instruments_[kSym] = std::make_shared<FuturesInstrument>(kSym, spec);
        registry.initialized_ = true;
        registry_ = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
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
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
        TestBase::TearDown();
    }

    // Production shape: vol_lookback_long 252, so the history holds 756 bars.
    void make_strategy() {
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params[kSym] = 50.0;
        sc.position_limits[kSym] = 1000.0;

        typename Traits<S>::Config tc;
        tc.weight = 1.0;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.use_position_buffering = false;
        tc.vol_lookback_short = Traits<S>::kVolSpan;
        tc.vol_lookback_long = 252;

        static int id = 0;
        strategy_ = std::make_unique<S>("TEST_VOL_ANN_" + std::to_string(++id), sc, tc, db_,
                                        registry_);
        ASSERT_TRUE(strategy_->initialize().is_ok());
        RiskLimits rl;
        rl.max_position_size = 1000.0;
        rl.max_notional_value = 1'000'000'000.0;
        rl.max_drawdown = 0.5;
        rl.max_leverage = 100.0;
        ASSERT_TRUE(strategy_->update_risk_limits(rl).is_ok());
        ASSERT_TRUE(strategy_->start().is_ok());
    }

    // The first 900 bars in one call (the live runner's bulk load), the rest one a day (the
    // backtest's daily feed), so both history paths are exercised.
    void feed(const std::vector<Bar>& bars) {
        const size_t bulk = std::min<size_t>(900, bars.size());
        ASSERT_TRUE(
            strategy_->on_data(std::vector<Bar>(bars.begin(), bars.begin() + bulk)).is_ok());
        for (size_t k = bulk; k < bars.size(); ++k) {
            ASSERT_TRUE(strategy_->on_data({bars[k]}).is_ok());
        }
    }

    double vol_after(const std::vector<Bar>& bars) {
        make_strategy();
        feed(bars);
        const auto* data = strategy_->get_instrument_data(kSym);
        EXPECT_NE(data, nullptr);
        return data ? data->current_volatility : 0.0;
    }

    double forecast_after(const std::vector<Bar>& bars) {
        make_strategy();
        feed(bars);
        return strategy_->get_forecast(kSym);
    }

    std::shared_ptr<MockPostgresDatabase> db_;
    std::shared_ptr<InstrumentRegistry> registry_;
    std::unique_ptr<S> strategy_;
};

using TrendStrategies =
    ::testing::Types<TrendFollowingStrategy, TrendFollowingFastStrategy, TrendFollowingSlowStrategy>;
TYPED_TEST_SUITE(VolAnnualisationTest, TrendStrategies);

// A six-bar week (Sunday session row) at a known weekly variance returns the known annual vol.
// A fixed 16 returns about 0.903 of it.
TYPED_TEST(VolAnnualisationTest, SixBarWeekReturnsKnownAnnualVol) {
    const double expected = known_annual_vol(Traits<TypeParam>::kVolSpan, 6);
    const double vol = this->vol_after(alternating_bars(calendar(997, 6)));
    EXPECT_NEAR(vol / expected, 1.0, kTolerance)
        << "vol=" << vol << " expected=" << expected
        << " (a fixed 16 gives about " << expected * 16.0 / std::sqrt(6 * kDaysPerYear / 7.0)
        << ")";
}

// A five-bar week returns the known annual vol: sqrt(5 x 365.25 / 7) = 16.152 bars' worth, so
// Carver's 16 was within one percent of right here.
TYPED_TEST(VolAnnualisationTest, FiveBarWeekReturnsKnownAnnualVol) {
    const double expected = known_annual_vol(Traits<TypeParam>::kVolSpan, 5);
    const double vol = this->vol_after(alternating_bars(calendar(997, 5)));
    EXPECT_NEAR(vol / expected, 1.0, kTolerance) << "vol=" << vol << " expected=" << expected;
}

// The count is taken over the bars the estimator reads (the last 756 here), not the whole feed:
// 400 hourly bars before the six-bar calendar would move a whole-feed count far off.
TYPED_TEST(VolAnnualisationTest, CountUsesTheEstimatorsWindowOnly) {
    std::vector<Timestamp> ts;
    for (int h = 0; h < 400; ++h) {
        ts.push_back(day(0) + std::chrono::hours(h));
    }
    const auto six = calendar(997, 6, /*first_day=*/21);
    ts.insert(ts.end(), six.begin(), six.end());
    const double expected = known_annual_vol(Traits<TypeParam>::kVolSpan, 6);
    const double vol = this->vol_after(alternating_bars(ts));
    EXPECT_NEAR(vol / expected, 1.0, kTolerance) << "vol=" << vol << " expected=" << expected;
}

// The forecast divides by the per-bar vol, which does not depend on the calendar: the same
// prices on a six-bar and a five-bar calendar give the same forecast, bit for bit.
TYPED_TEST(VolAnnualisationTest, ForecastDoesNotDependOnTheCalendar) {
    const double f6 = this->forecast_after(alternating_bars(calendar(997, 6)));
    this->strategy_->stop();
    this->strategy_.reset();
    const double f5 = this->forecast_after(alternating_bars(calendar(997, 5)));
    EXPECT_EQ(f6, f5);
}

// The counting rule itself.
TEST(VolAnnualisationRule, CountsReturnsPerCalendarYear) {
    std::deque<Timestamp> six;
    for (const auto& t : calendar(6 * 52 + 1, 6)) six.push_back(t);  // 52 whole weeks
    auto a = vol_annualisation(six, six.size());
    EXPECT_FALSE(a.fallback);
    EXPECT_EQ(a.bars, six.size());
    EXPECT_DOUBLE_EQ(a.span_days, 364.0);
    EXPECT_NEAR(a.bars_per_year, 6.0 * kDaysPerYear / 7.0, 1e-9);  // 313.07
    EXPECT_NEAR(a.factor, std::sqrt(6.0 * kDaysPerYear / 7.0), 1e-12);

    std::deque<Timestamp> five;
    for (const auto& t : calendar(5 * 52 + 1, 5)) five.push_back(t);
    auto b = vol_annualisation(five, five.size());
    EXPECT_NEAR(b.bars_per_year, 5.0 * kDaysPerYear / 7.0, 1e-9);  // 260.89
    EXPECT_NEAR(b.factor, 16.152, 5e-4);
}

TEST(VolAnnualisationRule, UsesOnlyTheLastWindowBars) {
    std::deque<Timestamp> ts;
    for (int h = 0; h < 50; ++h) ts.push_back(day(0) + std::chrono::hours(h));
    for (const auto& t : calendar(6 * 52 + 1, 6, 14)) ts.push_back(t);
    auto a = vol_annualisation(ts, 6 * 52 + 1);
    EXPECT_NEAR(a.bars_per_year, 6.0 * kDaysPerYear / 7.0, 1e-9);
    // A window longer than the history counts the whole history.
    auto all = vol_annualisation(ts, 10'000);
    EXPECT_EQ(all.bars, ts.size());
}

// A holiday-thin five-bar year (nine interior weekday closures) keeps 252 bars over the same
// 364 days: 251 returns a year x 365.25 / 364 = 251.86, factor 15.870.
TEST(VolAnnualisationRule, HolidayThinYearCountsFewerBars) {
    std::deque<Timestamp> ts;
    const auto five = calendar(5 * 52 + 1, 5);
    for (size_t k = 0; k < five.size(); ++k) {
        if (k % 25 == 20 && k < 230) {
            continue;  // closures at k = 20, 45, ..., 220
        }
        ts.push_back(five[k]);
    }
    ASSERT_EQ(ts.size(), 252u);
    auto a = vol_annualisation(ts, ts.size());
    EXPECT_DOUBLE_EQ(a.span_days, 364.0);
    EXPECT_NEAR(a.bars_per_year, 251.0 * kDaysPerYear / 364.0, 1e-9);
    EXPECT_NEAR(a.factor, 15.870, 5e-4);
}

TEST(VolAnnualisationRule, FallsBackToSixteenWhenItCannotCount) {
    std::deque<Timestamp> empty;
    EXPECT_TRUE(vol_annualisation(empty, 756).fallback);
    EXPECT_DOUBLE_EQ(vol_annualisation(empty, 756).factor, kCarverAnnualisation);

    std::deque<Timestamp> one{day(0)};
    EXPECT_DOUBLE_EQ(vol_annualisation(one, 756).factor, 16.0);

    std::deque<Timestamp> same{day(3), day(3), day(3)};  // zero span
    auto z = vol_annualisation(same, 756);
    EXPECT_TRUE(z.fallback);
    EXPECT_DOUBLE_EQ(z.factor, 16.0);

    std::deque<Timestamp> two{day(0), day(1)};
    EXPECT_DOUBLE_EQ(vol_annualisation(two, 1).factor, 16.0);  // window of one bar
    EXPECT_NEAR(vol_annualisation(two, 2).factor, std::sqrt(kDaysPerYear), 1e-12);
}
