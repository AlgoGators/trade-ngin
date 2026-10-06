// The estimators' window reaches back W consumed bars, further than a run's own bar window
// (LOOP_SPEC v6.2 section 1, "History and seeds"):
//   - the trend sleeve keeps the last W bars and reads all of them (it used to read the last 1,000);
//   - the bars before a run's window are seeded into the sleeve's history (seed_history), where a
//     bulk feed keeps them in front of the fed bars and a daily feed appends to them, so a seeded
//     sleeve computes what a sleeve fed the whole history computes;
//   - the published forecast is the fixed-scalar, attenuated forecast of section 2.5;
//   - a pair without a fixed scalar is refused;
//   - the PortfolioManager hands the seed to its sleeves; both engines build it from the bars before
//     their window with K-01 applied (live_estimator_history.hpp) and seed before they feed.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "../risk/risk_module_test_helpers.hpp"
#include "trade_ngin/instruments/futures.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::string kSym = "TSTSEED";
const Timestamp kDay0 = std::chrono::system_clock::from_time_t(1262563200);  // Monday 2010-01-04

Timestamp day(int d) { return kDay0 + std::chrono::hours(24 * d); }

// n bars from bar index `first`, one a calendar day; a deterministic walk whose volatility swells
// and fades (so the attenuation moves) around a slow trend. `tilt` changes the path of the bars
// whose index is below `tilt_below`.
std::vector<Bar> walk(int first, int n, double tilt = 0.0, int tilt_below = 0) {
    std::vector<Bar> bars;
    double p = 100.0;
    unsigned state = 2463534242u;
    for (int k = 0; k < first + n; ++k) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double u = static_cast<double>(state % 100000u) / 100000.0 - 0.5;
        const double swell = 1.0 + 0.7 * std::sin(k / 61.0);
        p *= 1.0 + 0.012 * swell * u + 0.0003 + (k < tilt_below ? tilt : 0.0);
        if (k < first) continue;
        Bar b;
        b.symbol = kSym;
        b.timestamp = day(k);
        b.open = Decimal(p);
        b.high = Decimal(p * 1.002);
        b.low = Decimal(p * 0.998);
        b.close = Decimal(p);
        b.volume = 100000.0;
        b.instrument_id = "A";
        bars.push_back(b);
    }
    return bars;
}

struct Sleeve {
    std::shared_ptr<MockPostgresDatabase> db;
    std::unique_ptr<TrendFollowingStrategy> strategy;

    explicit Sleeve(const std::string& id, std::vector<std::pair<int, int>> pairs = {{2, 8},
                                                                                     {4, 16},
                                                                                     {8, 32},
                                                                                     {16, 64}},
                    bool expect_initialized = true) {
        db = std::make_shared<MockPostgresDatabase>("mock://testdb");
        EXPECT_TRUE(db->connect().is_ok());
        StrategyConfig sc;
        sc.capital_allocation = 1'000'000.0;
        sc.max_leverage = 100.0;
        sc.asset_classes = {AssetClass::FUTURES};
        sc.frequencies = {DataFrequency::DAILY};
        sc.trading_params[kSym] = 5.0;
        sc.position_limits[kSym] = 1000.0;
        TrendFollowingConfig tc;
        tc.risk_target = 0.2;
        tc.idm = 2.5;
        tc.ema_windows = std::move(pairs);
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
        spec.trading_hours = "09:30-16:00";
        registry.instruments_[kSym] = std::make_shared<FuturesInstrument>(kSym, spec);
        registry.initialized_ = true;
        auto registry_ptr = std::shared_ptr<InstrumentRegistry>(&registry, [](InstrumentRegistry*) {});
        strategy = std::make_unique<TrendFollowingStrategy>(id, sc, tc, db, registry_ptr);
        initialized = strategy->initialize();
        if (!expect_initialized) return;
        EXPECT_TRUE(initialized.is_ok());
        RiskLimits limits;
        limits.max_position_size = 1000.0;
        limits.max_notional_value = 1e9;
        limits.max_drawdown = 0.5;
        limits.max_leverage = 100.0;
        EXPECT_TRUE(strategy->update_risk_limits(limits).is_ok());
        EXPECT_TRUE(strategy->start().is_ok());
    }
    ~Sleeve() {
        strategy->stop();
        strategy.reset();
        db->disconnect();
        auto& registry = InstrumentRegistry::instance();
        registry.instruments_.clear();
        registry.initialized_ = false;
    }
    const InstrumentData& data() const { return strategy->get_all_instrument_data().at(kSym); }
    Result<void> initialized;
};

std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "config_template")) return dir;
        if (dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return {};
}

std::string read_all(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The section 2.5 forecast of the last bar of `bars` (no contract switch: the level is the close),
// by the plain definition, for the pairs (2,8), (4,16), (8,32), (16,64), vol span 32, FDM 1.13.
double plain_forecast(const std::vector<Bar>& bars) {
    const size_t n = bars.size();
    std::vector<double> close(n), r(n, 0.0);
    for (size_t k = 0; k < n; ++k) close[k] = static_cast<double>(bars[k].close);
    for (size_t k = 1; k < n; ++k) r[k] = (close[k] - close[k - 1]) / close[k - 1];
    const double lambda = 2.0 / 33.0;
    std::vector<double> sizing(n, 0.0), forecast(n, 0.0);
    double mean = r[1], var = std::max(0.1 * r[1] * r[1], 1e-6);
    auto factor = [&](size_t k) {  // one bar a calendar day: bars a year over the trailing 256 bars
        const size_t s = k + 1 > 256 ? k + 1 - 256 : 0;
        return k == s ? 16.0 : std::sqrt(365.25);
    };
    sizing[1] = std::clamp(std::sqrt(var) * factor(1), 0.005, 5.0);
    forecast[1] = std::clamp(std::sqrt(var) * 16.0, 0.005, 5.0);
    for (size_t k = 2; k < n; ++k) {
        mean = lambda * r[k] + (1.0 - lambda) * mean;
        var = std::max(lambda * (r[k] - mean) * (r[k] - mean) + (1.0 - lambda) * var, 1e-6);
        sizing[k] = std::clamp(std::sqrt(var) * factor(k), 0.005, 5.0);
        forecast[k] = std::clamp(std::sqrt(var) * 16.0, 0.005, 5.0);
    }
    const size_t last = n - 1;
    const size_t lo = std::max<size_t>(last + 1 > 2520 ? last + 1 - 2520 : 0, 1);
    double fsum = 0.0;
    for (size_t k = lo; k <= last; ++k) fsum += forecast[k];
    const double forecast_vol = 0.7 * forecast[last] + 0.3 * fsum / static_cast<double>(last - lo + 1);
    double smoothed = 2.0 / 3.0;
    bool seeded = false;
    for (size_t c = 252; c <= last; ++c) {
        const size_t from = std::max<size_t>(c + 1 > 2520 ? c + 1 - 2520 : 0, 1);
        size_t at_or_below = 0;
        for (size_t k = from; k <= c; ++k) at_or_below += sizing[k] <= sizing[c] ? 1 : 0;
        const double q = static_cast<double>(at_or_below) / static_cast<double>(c - from + 1);
        smoothed = seeded ? (2.0 / 11.0) * q + (9.0 / 11.0) * smoothed : q;
        seeded = true;
    }
    const double attenuation = seeded ? 2.0 - 1.5 * smoothed : 1.0;
    const std::vector<std::pair<int, double>> pairs = {{2, 12.1}, {4, 8.53}, {8, 5.95}, {16, 4.10}};
    double total = 0.0;
    for (const auto& [fast, scalar] : pairs) {
        const double lf = 2.0 / (fast + 1.0), ls = 2.0 / (4.0 * fast + 1.0);
        double ef = close[0], es = close[0];
        for (size_t k = 1; k < n; ++k) {
            ef = lf * close[k] + (1.0 - lf) * ef;
            es = ls * close[k] + (1.0 - ls) * es;
        }
        total += std::clamp((ef - es) / (close[last] * forecast_vol / 16.0) * attenuation * scalar,
                            -20.0, 20.0);
    }
    return std::clamp(1.13 * total / 4.0, -20.0, 20.0);
}

}  // namespace

// The published forecast is section 2.5's: fixed scalars, the attenuation, the multiplier, the caps.
TEST(TrendHistorySeed, ThePublishedForecastIsTheFixedScalarForecast) {
    StateManager::reset_instance();
    const auto bars = walk(0, 600);
    Sleeve sleeve("SEED_fixed_scalar");
    ASSERT_TRUE(sleeve.strategy->on_data(bars).is_ok());
    const double expected = plain_forecast(bars);
    ASSERT_GT(std::abs(expected), 0.05) << "the fixture's forecast is not informative";
    EXPECT_NEAR(sleeve.strategy->get_forecast(kSym), expected, 1e-9 * std::max(1.0, std::abs(expected)));
}

// Two histories of 1,500 bars that differ only in their first 400: the estimators read the whole
// window (W = 3,200 bars), so the forecasts differ. (A sleeve that read its last 1,000 bars alone
// could not tell them apart.)
TEST(TrendHistorySeed, TheEstimatorsReadMoreThanTheLastThousandBars) {
    StateManager::reset_instance();
    const auto plain = walk(0, 1500);
    const auto tilted = walk(0, 1500, 0.002, 400);
    // The paths' last 1,100 bars have the same RETURNS; the levels differ by the earlier tilt.
    Sleeve a("SEED_window_a");
    Sleeve b("SEED_window_b");
    ASSERT_TRUE(a.strategy->on_data(plain).is_ok());
    ASSERT_TRUE(b.strategy->on_data(tilted).is_ok());
    ASSERT_EQ(a.data().price_history.size(), 1500u);
    EXPECT_NE(a.data().current_volatility, b.data().current_volatility)
        << "the long-run mean of the volatility reads the first 400 bars";
}

TEST(TrendHistorySeed, APairWithoutAFixedScalarIsRefused) {
    StateManager::reset_instance();
    Sleeve sleeve("SEED_one_four", {{1, 4}, {2, 8}, {4, 16}}, /*expect_initialized=*/false);
    ASSERT_TRUE(sleeve.initialized.is_error()) << "the (1,4) pair has no fixed scalar";
    EXPECT_NE(std::string(sleeve.initialized.error()->what()).find("(1, 4)"), std::string::npos);
}

// A sleeve seeded with the bars before its window and then fed the window, daily or in bulk,
// computes exactly what a sleeve fed every bar computes.
TEST(TrendHistorySeed, ASeededSleeveEqualsASleeveFedTheWholeHistory) {
    StateManager::reset_instance();
    const auto all = walk(0, 900);
    const std::vector<Bar> before(all.begin(), all.begin() + 500);
    const std::vector<Bar> window(all.begin() + 500, all.end());

    Sleeve whole("SEED_whole");
    ASSERT_TRUE(whole.strategy->on_data(all).is_ok());

    Sleeve bulk("SEED_bulk");  // the live shape: seed, then one bulk feed of the window (400 bars)
    ASSERT_TRUE(bulk.strategy->seed_history(before).is_ok());
    EXPECT_EQ(bulk.data().price_history.size(), 500u);
    EXPECT_TRUE(bulk.strategy->get_target_positions().at(kSym).quantity == Decimal(0.0))
        << "seeding sizes nothing";
    ASSERT_TRUE(bulk.strategy->on_data(window).is_ok());
    EXPECT_EQ(bulk.data().price_history.size(), 900u);
    EXPECT_EQ(bulk.data().current_volatility, whole.data().current_volatility);
    EXPECT_EQ(bulk.data().current_forecast, whole.data().current_forecast);

    Sleeve daily("SEED_daily");  // the backtest shape: seed, then one bar a cycle
    ASSERT_TRUE(daily.strategy->seed_history(before).is_ok());
    for (const auto& b : window) ASSERT_TRUE(daily.strategy->on_data({b}).is_ok());
    EXPECT_EQ(daily.data().price_history.size(), 900u);
    EXPECT_EQ(daily.data().current_volatility, whole.data().current_volatility);
    EXPECT_EQ(daily.data().current_forecast, whole.data().current_forecast);
}

// A bulk feed still replaces the FED history; only the seed stays, and only its bars dated before
// the feed's first bar.
TEST(TrendHistorySeed, ABulkFeedKeepsTheSeedInFrontAndReplacesTheRest) {
    StateManager::reset_instance();
    const auto all = walk(0, 700);
    Sleeve sleeve("SEED_replace");
    // The seed overlaps the feed by 50 bars: the overlapping ones give way to the feed's.
    ASSERT_TRUE(sleeve.strategy->seed_history(std::vector<Bar>(all.begin(), all.begin() + 350)).is_ok());
    ASSERT_TRUE(sleeve.strategy->on_data(std::vector<Bar>(all.begin() + 300, all.begin() + 600)).is_ok());
    ASSERT_EQ(sleeve.data().price_history.size(), 600u);
    for (size_t k = 0; k < 600; ++k) {
        ASSERT_EQ(sleeve.data().bar_timestamps[k], all[k].timestamp) << "bar " << k;
    }
    // A second bulk feed replaces the first one's bars and keeps the seed's.
    ASSERT_TRUE(sleeve.strategy->on_data(std::vector<Bar>(all.begin() + 320, all.end())).is_ok());
    ASSERT_EQ(sleeve.data().price_history.size(), 700u);
    EXPECT_EQ(sleeve.data().bar_timestamps[319], all[319].timestamp);
    EXPECT_EQ(sleeve.data().bar_timestamps[320], all[320].timestamp);

    // Without a seed a bulk feed replaces everything, as before.
    Sleeve plain("SEED_plain");
    ASSERT_TRUE(plain.strategy->on_data(std::vector<Bar>(all.begin(), all.begin() + 300)).is_ok());
    ASSERT_TRUE(plain.strategy->on_data(std::vector<Bar>(all.begin() + 300, all.end())).is_ok());
    EXPECT_EQ(plain.data().price_history.size(), 400u);
}

// The history is kept to the window's length, the oldest bars dropped first.
TEST(TrendHistorySeed, TheHistoryIsKeptToTheWindow) {
    StateManager::reset_instance();
    const int n = static_cast<int>(trend_estimator::kWindowBars) + 250;
    const auto all = walk(0, n);
    Sleeve sleeve("SEED_cap");
    ASSERT_TRUE(sleeve.strategy->seed_history(std::vector<Bar>(all.begin(), all.end() - 200)).is_ok());
    EXPECT_EQ(sleeve.data().price_history.size(), trend_estimator::kWindowBars);
    for (auto it = all.end() - 200; it != all.end(); ++it) {
        ASSERT_TRUE(sleeve.strategy->on_data({*it}).is_ok());
    }
    EXPECT_EQ(sleeve.data().price_history.size(), trend_estimator::kWindowBars);
    EXPECT_EQ(sleeve.data().bar_timestamps.back(), all.back().timestamp);
    EXPECT_EQ(sleeve.data().bar_timestamps.front(), all[n - trend_estimator::kWindowBars].timestamp);
    EXPECT_EQ(sleeve.data().estimate.window_bars, trend_estimator::kWindowBars);
}

// ---- the engines' side: what is seeded, and that it is seeded before the feed ----------------

#include "trade_ngin/live/live_estimator_history.hpp"

namespace {

class SeedRecordingStrategy : public BaseStrategy {
public:
    SeedRecordingStrategy(std::string id, StrategyConfig config, std::shared_ptr<DatabaseInterface> db)
        : BaseStrategy(std::move(id), std::move(config),
                       std::static_pointer_cast<trade_ngin::PostgresDatabase>(db)) {
        metadata_.name = "Seed Recording Strategy";
    }
    Result<void> on_data(const std::vector<Bar>& data) override {
        fed += data.size();
        return Result<void>();
    }
    Result<void> seed_history(const std::vector<Bar>& bars) override {
        seeded.push_back(bars.size());
        return Result<void>();
    }
    std::unordered_map<std::string, Position> get_target_positions() const override { return {}; }
    std::vector<size_t> seeded;
    size_t fed = 0;
};

}  // namespace

// The PortfolioManager hands the seed to every sleeve; a strategy that keeps no such history
// ignores it; nothing is fed.
TEST(TrendHistorySeed, ThePortfolioManagerHandsTheSeedToItsSleeves) {
    StateManager::reset_instance();
    auto db = std::make_shared<MockPostgresDatabase>("mock://testdb");
    ASSERT_TRUE(db->connect().is_ok());
    PortfolioConfig pc{1'000'000.0, 1.0, 0.0, false};
    pc.opt_config.capital = 1'000'000.0;
    pc.risk_config.capital = 1'000'000.0;
    pc.risk_modules = {test_none_module()};
    auto pm = std::make_shared<PortfolioManager>(pc, "PM_SEED_TEST");
    StrategyConfig sc;
    sc.capital_allocation = 500'000.0;
    sc.max_leverage = 10.0;
    sc.asset_classes = {AssetClass::FUTURES};
    sc.frequencies = {DataFrequency::DAILY};
    auto a = std::make_shared<SeedRecordingStrategy>("SEED_A", sc, db);
    auto b = std::make_shared<SeedRecordingStrategy>("SEED_B", sc, db);
    for (auto& s : {a, b}) {
        ASSERT_TRUE(s->initialize().is_ok());
        ASSERT_TRUE(s->start().is_ok());
    }
    ASSERT_TRUE(pm->add_strategy(a, 0.5, false).is_ok());
    ASSERT_TRUE(pm->add_strategy(b, 0.5, false).is_ok());
    const auto bars = walk(0, 40);
    ASSERT_TRUE(pm->seed_strategy_history(bars).is_ok());
    EXPECT_EQ(a->seeded, (std::vector<size_t>{40}));
    EXPECT_EQ(b->seeded, (std::vector<size_t>{40}));
    EXPECT_EQ(a->fed, 0u);
    EXPECT_EQ(b->fed, 0u);
    pm.reset();
    StateManager::reset_instance();
}

// The seed is the CONSUMED bars dated from the history start and strictly before the window: a bar
// the classifier withholds (K-01; a locked bar here) is never in it, nor is a bar of the window, nor
// a bar older than the history start.
TEST(TrendHistorySeed, TheSeedIsTheConsumedBarsBeforeTheWindow) {
    const Timestamp window_start = day(8000);
    auto all = walk(7900, 120);  // bars 7900..8019: 100 before the window, 20 inside it
    auto& locked = all[60];      // bar 7960, before the window
    locked.high = locked.close;
    locked.low = locked.close;
    locked.open = locked.close;
    Bar ancient = all.front();
    ancient.timestamp = estimator_history_start(window_start) - std::chrono::hours(24);
    all.insert(all.begin(), ancient);
    std::vector<SymbolDayVerdict> withheld;
    const auto seed = estimator_history_consumed(
        all, window_start,
        make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
            ErrorCode::NOT_INITIALIZED, "no ids in this fixture", "test"),
        &withheld);
    EXPECT_EQ(seed.size(), 99u) << "100 bars before the window, one of them withheld";
    for (const auto& b : seed) {
        EXPECT_LT(b.timestamp, window_start);
        EXPECT_GE(b.timestamp, estimator_history_start(window_start));
        EXPECT_NE(b.timestamp, day(7960)) << "the locked bar is withheld";
    }
    ASSERT_EQ(withheld.size(), 1u);
    EXPECT_EQ(withheld[0].symbol, kSym);
    EXPECT_EQ(estimator_history_start(window_start),
              window_start - std::chrono::hours(24 * trend_estimator::kHistoryCalendarDays));
}

// A window starts at local midnight, some hours into a UTC date, and its query reads the bars
// STORED at or after that instant: the bar of the date the window starts inside is stored at that
// date's midnight UTC, before the window, so the window does not load it. The loader stamps a bar
// some hours after its stored midnight; the split reads the stored date, so that bar is history
// and no bar falls between the history and the window.
TEST(TrendHistorySeed, TheBarOfTheDateTheWindowStartsInsideIsHistory) {
    const Timestamp window_start = day(8000) + std::chrono::hours(4);
    auto all = walk(7990, 20);  // bars 7990..8009
    for (auto& b : all) b.timestamp += std::chrono::hours(10);  // the loader's stamp
    const auto seed = estimator_history_consumed(
        all, window_start,
        make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
            ErrorCode::NOT_INITIALIZED, "no ids in this fixture", "test"));
    ASSERT_EQ(seed.size(), 11u) << "the bars stored on 7990..8000";
    EXPECT_EQ(seed.back().timestamp, day(8000) + std::chrono::hours(10))
        << "the bar stored at the midnight before the window's first instant is history";
    // A bar stored at the window's first instant or after it is the window's.
    const auto at_midnight = estimator_history_consumed(
        all, day(8000),
        make_error<std::vector<market_data_utils::FuturesInstrumentId>>(
            ErrorCode::NOT_INITIALIZED, "no ids in this fixture", "test"));
    EXPECT_EQ(at_midnight.size(), 10u);
}

// Both live runners and the backtest seed the sleeves before they feed them.
TEST(TrendHistorySeed, EveryEngineSeedsBeforeItFeeds) {
    const auto root = repo_root();
    if (root.empty()) GTEST_SKIP() << "repository root not found from " << std::filesystem::current_path();
    for (const auto* runner :
         {"apps/strategies/live_portfolio.cpp", "apps/strategies/live_portfolio_conservative.cpp"}) {
        const std::string src = read_all(root / runner);
        const size_t load = src.find("estimator_history_start(start_date), start_date,");
        const size_t build = src.find("estimator_history_bars = estimator_history_consumed(");
        const size_t seed = src.find("portfolio->seed_strategy_history(estimator_history_bars)");
        const size_t feed = src.find("portfolio->process_market_data(strategy_feed_bars)");
        ASSERT_NE(load, std::string::npos) << runner;
        ASSERT_NE(build, std::string::npos) << runner;
        ASSERT_NE(seed, std::string::npos) << runner;
        ASSERT_NE(feed, std::string::npos) << runner;
        EXPECT_LT(load, build) << runner;
        EXPECT_LT(build, seed) << runner;
        EXPECT_LT(seed, feed) << runner << ": the seed must precede the window's feed";
    }
    const std::string coordinator = read_all(root / "src/backtest/backtest_coordinator.cpp");
    const size_t run = coordinator.find("BacktestCoordinator::run_portfolio(");
    ASSERT_NE(run, std::string::npos);
    const size_t seed = coordinator.find(
        "seed_estimator_history(portfolio, symbols, start_date, asset_class, data_freq)", run);
    const size_t loop = coordinator.find("for (const auto& [timestamp, bars] : grouped_bars) {", run);
    ASSERT_NE(seed, std::string::npos);
    ASSERT_NE(loop, std::string::npos);
    EXPECT_LT(seed, loop) << "the backtest seeds before its first cycle";
    EXPECT_NE(coordinator.find("return portfolio->seed_strategy_history(history);"), std::string::npos);
}

// A seeded sleeve has a forecast on every warm-up cycle, so warm-up ends with targets no fill
// stands behind. The backtest's roll legs are booked at the FILLED book at the start of the bar:
// a roll confirmed on the first traded cycle has no leg.
TEST(TrendHistorySeed, TheBacktestRollsTheFilledBook) {
    const std::string coordinator =
        read_all(repo_root() / "src/backtest/backtest_coordinator.cpp");
    const size_t book = coordinator.find("const auto start_of_bar_book = confirmed_now.empty()");
    ASSERT_NE(book, std::string::npos);
    const size_t end = coordinator.find(';', book);
    const std::string statement = coordinator.substr(book, end - book);
    EXPECT_NE(statement.find("portfolio->get_filled_strategy_positions()"), std::string::npos);
    EXPECT_EQ(statement.find("portfolio->get_strategy_positions()"), std::string::npos);
}
