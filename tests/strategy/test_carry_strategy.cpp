// Carry in the trend strategy (trade-ngin #185): the hand-over from portfolio.json to the first
// sleeve, the runners' calls, the sleeve's own checks of its carry settings, and the blended
// forecast the sleeve trades: carry off is today's to the last digit, an instrument with no quote
// trades its trend alone, the blend is table 51 and 52's, and the equity slow rule acts on the
// trend part only.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/data/listing_dates.hpp"
#include "trade_ngin/instruments/futures.hpp"

#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/trend_following.hpp"
#undef private
#include "trade_ngin/strategy/carry_log.hpp"
#include "trade_ngin/strategy/sleeve_config.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

TEST(CarryConfig, TheHandOverCarriesTheBlockOnlyWhenCarryIsOn) {
    AppConfig app;
    app.carry.present = true;
    app.carry.weight = 0.4;
    app.carry.spans = {20, 60, 120};
    app.carry.scalar = 25.0;
    TrendFollowingConfig handed;
    hand_over_carry(app, handed);
    EXPECT_EQ(handed.carry_weight, 0.4);
    EXPECT_EQ(handed.carry_spans, (std::vector<int>{20, 60, 120}));
    EXPECT_EQ(handed.carry_scalar, 25.0);
    // weight 0 and an absent block hand over carry off
    app.carry.weight = 0.0;
    TrendFollowingConfig off;
    off.carry_weight = 0.4;
    hand_over_carry(app, off);
    EXPECT_EQ(off.carry_weight, 0.0);
    TrendFollowingConfig absent;
    hand_over_carry(AppConfig{}, absent);
    EXPECT_EQ(absent.carry_weight, 0.0);
    EXPECT_EQ(absent.carry_spans, (std::vector<int>{5, 20, 60, 120}));
}

// Every futures runner hands the block to its first trend sleeve, once, beside the trading-rule
// removals, and tells the bar source to carry the quotes when carry is on.
TEST(CarryConfig, EveryFuturesRunnerCallsTheHandOverAndTurnsTheQuotesOn) {
    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") &&
             std::filesystem::exists(root / "config_template"))) {
        ASSERT_NE(root, root.parent_path()) << "repository root not found";
        root = root.parent_path();
    }
    auto count = [](const std::string& text, const std::string& what) {
        std::size_t n = 0;
        for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
        return n;
    };
    for (const char* runner : {"apps/backtest/bt_portfolio.cpp", "apps/backtest/bt_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp",
                               "apps/strategies/live_portfolio_conservative.cpp"}) {
        std::ifstream in(root / runner);
        ASSERT_TRUE(in.good()) << runner;
        std::stringstream text;
        text << in.rdbuf();
        const std::string source = text.str();
        EXPECT_EQ(count(source, "hand_over_carry(app_config, trend_config);"), 1u) << runner;
        EXPECT_EQ(count(source, "carry_weight ="), 0u) << runner << " sets the weight itself";
        const std::size_t removals = source.find("hand_over_trading_rule_removals(app_config, trend_config);");
        const std::size_t carry = source.find("hand_over_carry(app_config, trend_config);");
        const std::size_t fast = source.find("strategy_type == \"TrendFollowingFastStrategy\"");
        ASSERT_NE(removals, std::string::npos) << runner;
        EXPECT_LT(removals, carry) << runner;
        EXPECT_LT(carry, fast) << runner;
        EXPECT_EQ(count(source, "app_config.per_contract_rolls, app_config.per_contract_switch_bar,\n"
                                "                app_config.carry.enabled());"),
                  1u)
            << runner;
    }
}

TEST(CarryConfig, TheSleeveRefusesCarrySettingsItCannotRun) {
    TrendFollowingConfig ok;
    ok.carry_weight = 0.4;
    EXPECT_TRUE(TrendFollowingStrategy::validate_trend_config(ok).is_ok());
    ok.rule_removals = {{"ZR", {{2, 8}, {4, 16}, {8, 32}}}};
    EXPECT_TRUE(TrendFollowingStrategy::validate_trend_config(ok).is_ok()) << "7 rules: 1.29";
    TrendFollowingConfig off;  // the default is off and always valid
    EXPECT_EQ(off.carry_weight, 0.0);
    EXPECT_TRUE(TrendFollowingStrategy::validate_trend_config(off).is_ok());
    auto refused = [](TrendFollowingConfig c, const std::string& named) {
        auto r = TrendFollowingStrategy::validate_trend_config(c);
        ASSERT_TRUE(r.is_error());
        EXPECT_NE(std::string(r.error()->what()).find(named), std::string::npos) << r.error()->what();
    };
    TrendFollowingConfig c;
    c.carry_weight = 1.2;
    refused(c, "from 0 to 1");
    c.carry_weight = -0.1;
    refused(c, "from 0 to 1");
    c.carry_weight = 0.4;
    c.carry_spans = {};
    refused(c, "no span");
    c.carry_spans = {20, 5};
    refused(c, "fastest first");
    c.carry_spans = {5, 20};
    c.carry_scalar = 0.0;
    refused(c, "scalar");
    // 20 trend pairs are not supported anyway; a rule count table 52 does not print is refused:
    // six trend speeds and 17 carry spans are 23 rules.
    c.carry_scalar = 30.0;
    c.carry_spans.clear();
    for (int s = 1; s <= 17; ++s) c.carry_spans.push_back(s * 5);
    refused(c, "no multiplier for 23 trading rules");
}

namespace {

const std::string kSym = "TSTCARRY";
const std::vector<std::pair<int, int>> kSix = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
const Timestamp kDay0 = std::chrono::system_clock::from_time_t(1262563200);  // Monday 2010-01-04

// n daily bars drifting by `drift` (down after `up` bars by `fall`), a deterministic wobble, and a
// carry quote on every bar: the neighbour `spread` (a share of the price) above the held contract,
// one month nearer, so the annualised carry is 12 x spread x price. A bar k with quoted(k) false has
// no quote.
std::vector<Bar> path(int n, int up, double drift, double fall, double spread,
                      const std::function<bool(int)>& quoted = [](int) { return true; }) {
    std::vector<Bar> bars;
    double p = 100.0;
    unsigned state = 88172645u;
    for (int k = 0; k < n; ++k) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double u = static_cast<double>(state % 100000u) / 100000.0 - 0.5;
        p *= 1.0 + 0.006 * u + (k < up ? drift : -fall);
        Bar b;
        b.symbol = kSym;
        b.timestamp = kDay0 + std::chrono::hours(24 * k);
        b.open = Decimal(p);
        b.high = Decimal(p * 1.002);
        b.low = Decimal(p * 0.998);
        b.close = Decimal(p);
        b.volume = 100000.0;
        b.instrument_id = "A";
        if (quoted(k)) b.carry = CarryQuote{p, p * (1.0 + spread), -1, "TSTZ0", "TSTX0"};
        bars.push_back(b);
    }
    return bars;
}

struct Sleeve {
    std::shared_ptr<MockPostgresDatabase> db;
    std::unique_ptr<TrendFollowingStrategy> strategy;

    Sleeve(const std::string& id, double carry_weight, const std::vector<std::string>& ruled = {},
           const std::map<std::string, std::vector<std::pair<int, int>>>& removals = {}) {
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
        tc.ema_windows = kSix;
        tc.vol_lookback_short = 32;
        tc.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
        tc.equity_slow_symbols = ruled;
        tc.equity_slow_pairs = {{32, 128}, {64, 256}};
        tc.rule_removals = removals;
        tc.carry_weight = carry_weight;
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
        EXPECT_TRUE(strategy->initialize().is_ok());
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
    double forecast(const std::vector<Bar>& bars) {
        EXPECT_TRUE(strategy->on_data(bars).is_ok());
        return strategy->get_forecast(kSym);
    }
    const InstrumentData& data() const { return strategy->get_all_instrument_data().at(kSym); }
};

std::vector<Bar> without_quotes(std::vector<Bar> bars) {
    for (auto& b : bars) b.carry.reset();
    return bars;
}

}  // namespace

// Carry off (weight 0): the quotes on the bars are not read and every value is today's, bit for bit.
TEST(CarryStrategy, CarryOffIsTodaysStrategyToTheLastDigit) {
    StateManager::reset_instance();
    const auto bars = path(700, 400, 0.0012, 0.0006, 0.004);
    Sleeve today("CARRY_off_today", 0.0);
    Sleeve off("CARRY_off_quotes", 0.0);
    const double a = today.forecast(without_quotes(bars));
    const double b = off.forecast(bars);
    EXPECT_EQ(a, b);
    EXPECT_EQ(today.data().raw_position, off.data().raw_position);
    EXPECT_EQ(today.data().estimate.combined, off.data().estimate.combined);
    EXPECT_FALSE(off.data().carry_used);
    EXPECT_EQ(a, today.data().estimate.combined) << "the trend strategy's own combined forecast";
}

// Carry on and no quote anywhere in the window: the instrument trades its trend forecast alone
// (HD's Q3, per bar), the same value as carry off.
TEST(CarryStrategy, NoQuoteInTheWindowTradesTheTrendAlone) {
    StateManager::reset_instance();
    const auto bars = without_quotes(path(700, 400, 0.0012, 0.0006, 0.004));
    Sleeve off("CARRY_none_off", 0.0);
    Sleeve on("CARRY_none_on", 0.4);
    EXPECT_EQ(on.forecast(bars), off.forecast(bars));
    EXPECT_EQ(on.data().raw_position, off.data().raw_position);
    EXPECT_FALSE(on.data().carry_used);
    EXPECT_FALSE(on.data().carry.active);
}

// Carry on with quotes: F = 1.35 x (0.6 x the trend average + 0.4 x the carry average), capped, and
// the carry average is the carry estimator's on the sleeve's own window and volatility.
TEST(CarryStrategy, TheBlendIsTenRulesAtOnePointThreeFive) {
    StateManager::reset_instance();
    const auto bars = path(700, 400, 0.0012, 0.0006, 0.0008);
    Sleeve on("CARRY_blend", 0.4);
    const double f = on.forecast(bars);
    const InstrumentData& d = on.data();
    ASSERT_TRUE(d.carry_used);
    EXPECT_EQ(d.blend.rules, 10);
    EXPECT_EQ(d.blend.fdm, 1.35);
    const double hand = std::clamp(1.35 * (0.6 * d.estimate.mean_scaled + 0.4 * d.carry.mean_scaled), -20.0, 20.0);
    EXPECT_NEAR(f, hand, 1e-12);
    EXPECT_NE(f, d.estimate.combined) << "the blend moved the forecast";
    // The carry part on its own: annualised 12 x 0.0008 x price over price x forecast sigma.
    const auto& series_close = d.opt_closes;  // the last bars' raw closes
    ASSERT_FALSE(series_close.empty());
    const double price = series_close.back();
    EXPECT_NEAR(d.carry.annualised, 12.0 * 0.0008 * price, 1e-9);
    EXPECT_NEAR(d.carry.divisor, price * d.estimate.forecast_sigma, 1e-9);
    EXPECT_NEAR(d.carry.forecast, d.carry.annualised / d.carry.divisor, 1e-12);
    EXPECT_GT(d.carry.mean_scaled, 0.0);
    // the position is sized on the blended forecast
    EXPECT_NE(d.raw_position, 0.0);
}

// A contract that keeps three trend speeds trades seven rules: 1.29, trend weights 0.20 each.
TEST(CarryStrategy, ARemovalLeavesSevenRulesAtOnePointTwoNine) {
    StateManager::reset_instance();
    const auto bars = path(700, 400, 0.0012, 0.0006, 0.0008);
    Sleeve on("CARRY_seven", 0.4, {}, {{kSym, {{2, 8}, {4, 16}, {8, 32}}}});
    const double f = on.forecast(bars);
    const InstrumentData& d = on.data();
    ASSERT_TRUE(d.carry_used);
    EXPECT_EQ(d.blend.trend_rules, 3);
    EXPECT_EQ(d.blend.rules, 7);
    EXPECT_EQ(d.blend.fdm, 1.29);
    EXPECT_NEAR(d.blend.trend_weight, 0.2, 1e-15);
    EXPECT_NEAR(f, std::clamp(1.29 * (0.6 * d.estimate.mean_scaled + 0.4 * d.carry.mean_scaled), -20.0, 20.0), 1e-12);
}

// The equity slow rule acts on the trend part only (HD's Q1): a rise then a sharp fall makes the
// trend average negative with the slowest speed still positive, so the trend part is 0; positive
// carry then makes the forecast long, negative carry short (a carry-driven short, counted).
TEST(CarryStrategy, TheEquitySlowRuleActsOnTheTrendPartOnly) {
    StateManager::reset_instance();
    for (double spread : {0.0008, -0.0008}) {
        SCOPED_TRACE(spread);
        const auto bars = path(700, 670, 0.0015, 0.0080, spread);
        Sleeve ruled("CARRY_slow_" + std::to_string(spread > 0), 0.4, {kSym});
        const double f = ruled.forecast(bars);
        const InstrumentData& d = ruled.data();
        ASSERT_TRUE(d.carry_used);
        ASSERT_LT(d.estimate.mean_scaled, 0.0) << "the fixture's trend is not short";
        EXPECT_EQ(d.trend_part_ruled, 0.0) << "the trend part's short is ruled out";
        EXPECT_TRUE(d.slow_rule_zeroed);
        EXPECT_NEAR(f, 1.35 * 0.4 * d.carry.mean_scaled, 1e-12);
        if (spread > 0) {
            EXPECT_GT(f, 0.0);
            EXPECT_TRUE(ruled.strategy->carry_driven_shorts().empty());
        } else {
            EXPECT_LT(f, 0.0) << "a carry-driven equity short is allowed";
            ASSERT_EQ(ruled.strategy->carry_driven_shorts().count(kSym), 1u);
            EXPECT_EQ(ruled.strategy->carry_driven_shorts().at(kSym), 1u);
        }
    }
}

// The missing-price rule on the sleeve: the last bars have no quote, the last known raw carry stands.
TEST(CarryStrategy, TheLastKnownRawCarryStandsOnBarsWithNoQuote) {
    StateManager::reset_instance();
    const auto bars = path(700, 400, 0.0012, 0.0006, 0.0008, [](int k) { return k < 690; });
    Sleeve on("CARRY_missing", 0.4);
    on.forecast(bars);
    const InstrumentData& d = on.data();
    ASSERT_TRUE(d.carry_used);
    EXPECT_FALSE(d.carry.quoted);
    const double close_689 = static_cast<double>(bars[689].close);
    EXPECT_NEAR(d.carry.annualised, 12.0 * 0.0008 * close_689, 1e-9) << "bar 689's raw carry";
    EXPECT_NEAR(d.carry.divisor, static_cast<double>(bars.back().close) * d.estimate.forecast_sigma, 1e-9);
}

// The quotes travel with the seeded history: seeding the first bars and feeding the rest gives the
// forecast of feeding them all.
TEST(CarryStrategy, TheSeededHistoryKeepsItsQuotes) {
    StateManager::reset_instance();
    const auto bars = path(700, 400, 0.0012, 0.0006, 0.0008);
    Sleeve whole("CARRY_seed_whole", 0.4);
    Sleeve seeded("CARRY_seed_split", 0.4);
    const double a = whole.forecast(bars);
    ASSERT_TRUE(seeded.strategy->seed_history({bars.begin(), bars.begin() + 500}).is_ok());
    double b = 0.0;
    for (std::size_t k = 500; k < bars.size(); ++k) b = seeded.forecast({bars[k]});
    EXPECT_EQ(a, b);
    EXPECT_EQ(whole.data().carry.mean_scaled, seeded.data().carry.mean_scaled);
}

// The run's one line for carry-driven equity shorts (HD's Q1): printed only with carry on, counting the
// shorts the sleeve made; every futures runner prints it beside the short-window line.
TEST(CarryStrategy, TheRunReportsItsCarryDrivenEquityShorts) {
    StateManager::reset_instance();
    const auto bars = path(700, 670, 0.0015, 0.0080, -0.0008);
    auto off = std::make_shared<Sleeve>("CARRY_line_off", 0.0, std::vector<std::string>{kSym});
    off->forecast(bars);
    std::shared_ptr<StrategyInterface> off_ptr(off, off->strategy.get());
    EXPECT_EQ(carry_driven_shorts_line({off_ptr}), "") << "carry off prints nothing";
    auto on = std::make_shared<Sleeve>("CARRY_line_on", 0.4, std::vector<std::string>{kSym});
    on->forecast(bars);
    std::shared_ptr<StrategyInterface> on_ptr(on, on->strategy.get());
    EXPECT_EQ(carry_driven_shorts_line({on_ptr}), "CARRY_DRIVEN_EQUITY_SHORTS tradeable_bars symbols=1 rows=1 names=[TSTCARRY:1]");
    auto none = std::make_shared<Sleeve>("CARRY_line_none", 0.4);
    std::shared_ptr<StrategyInterface> none_ptr(none, none->strategy.get());
    EXPECT_EQ(carry_driven_shorts_line({none_ptr}), "CARRY_DRIVEN_EQUITY_SHORTS tradeable_bars symbols=0 rows=0 names=[-]");

    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") && std::filesystem::exists(root / "config_template"))) {
        ASSERT_NE(root, root.parent_path());
        root = root.parent_path();
    }
    for (const char* runner : {"apps/backtest/bt_portfolio.cpp", "apps/backtest/bt_portfolio_conservative.cpp",
                               "apps/strategies/live_portfolio.cpp", "apps/strategies/live_portfolio_conservative.cpp"}) {
        std::ifstream in(root / runner);
        std::stringstream text;
        text << in.rdbuf();
        const std::string source = text.str();
        const std::size_t short_line = source.find("INFO(trade_ngin::estimator_short_window_line(strategies));");
        const std::size_t carry_line = source.find("trade_ngin::carry_driven_shorts_line(strategies)");
        ASSERT_NE(carry_line, std::string::npos) << runner;
        EXPECT_LT(short_line, carry_line) << runner;
        EXPECT_LT(carry_line - short_line, 200u) << runner << ": printed beside the short-window line";
    }
}

// Only tradeable bars count (fix round 1, audits B and C): a contract outside its tradeable window
// (listed after these bars) computes its forecast on every bar but none of its shorts is counted.
TEST(CarryStrategy, OnlyTradeableBarsCountAsCarryDrivenShorts) {
    StateManager::reset_instance();
    const auto bars = path(700, 670, 0.0015, 0.0080, -0.0008);
    ListingDates::instance().set({{kSym, "TSTOLD", "2015-01-05", 10.0}});
    {
        Sleeve unlisted("CARRY_count_unlisted", 0.4, {kSym});
        unlisted.forecast(bars);
        EXPECT_TRUE(unlisted.data().carry_used) << "the forecast is still computed";
        EXPECT_TRUE(unlisted.strategy->carry_driven_shorts().empty())
            << "no tradeable bar, no carry-driven short counted";
    }
    ListingDates::instance().clear();
    Sleeve listed("CARRY_count_listed", 0.4, {kSym});
    listed.forecast(bars);
    ASSERT_EQ(listed.strategy->carry_driven_shorts().count(kSym), 1u);
    EXPECT_EQ(listed.strategy->carry_driven_shorts().at(kSym), 1u);
}
