// The equity slow rule (LOOP_SPEC v6.2 section 2.5, D40): on the symbols the rule names, a NEGATIVE
// combined forecast of a ruled sleeve stands only when the scaled forecast of every pair the rule
// names, (32,128) and (64,256), is negative; otherwise the forecast, and the position sized from
// it, is 0. A positive forecast, a symbol the rule does not name and a sleeve that is not ruled (the
// FAST sleeve, which carries neither pair) are untouched.
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

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

const std::string kSym = "TSTSLOW";
const std::vector<std::pair<int, int>> kSix = {{2, 8}, {4, 16}, {8, 32}, {16, 64}, {32, 128}, {64, 256}};
const std::vector<std::pair<int, int>> kFast = {{2, 8}, {4, 16}, {8, 32}, {16, 64}};
const std::vector<std::pair<int, int>> kSlowPairs = {{32, 128}, {64, 256}};
const Timestamp kDay0 = std::chrono::system_clock::from_time_t(1262563200);  // Monday 2010-01-04

// The rule's fields exist from the commit that adds the rule. Set through a constraint so that this
// file also compiles on the commit before it, where a sleeve cannot be ruled and the zeroing test
// fails: that run is the test's proof that it tests the rule.
template <class Config>
void rule(Config& config, const std::vector<std::string>& symbols,
          const std::vector<std::pair<int, int>>& pairs) {
    if constexpr (requires { config.equity_slow_symbols; }) {
        config.equity_slow_symbols = symbols;
        config.equity_slow_pairs = pairs;
    }
}

// n daily bars: `up` bars drifting up by `drift` a bar, then the rest drifting down by `fall` a bar,
// with a small deterministic wobble so the volatility is not on its floor.
std::vector<Bar> path(int n, int up, double drift, double fall) {
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
        bars.push_back(b);
    }
    return bars;
}

struct Sleeve {
    std::shared_ptr<MockPostgresDatabase> db;
    std::unique_ptr<TrendFollowingStrategy> strategy;
    Result<void> initialized;

    Sleeve(const std::string& id, const std::vector<std::pair<int, int>>& pairs,
           const std::vector<std::string>& ruled_symbols,
           const std::vector<std::pair<int, int>>& rule_pairs = kSlowPairs, bool start = true) {
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
        tc.ema_windows = pairs;
        tc.vol_lookback_short = pairs.size() == 4 ? 16 : 32;
        tc.fdm = {{1, 1.0}, {2, 1.03}, {3, 1.08}, {4, 1.13}, {5, 1.19}, {6, 1.26}};
        if (!ruled_symbols.empty()) rule(tc, ruled_symbols, rule_pairs);
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
        if (!start) return;
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
    double forecast(const std::vector<Bar>& bars) {
        EXPECT_TRUE(strategy->on_data(bars).is_ok());
        return strategy->get_forecast(kSym);
    }
    double position() const { return strategy->get_all_instrument_data().at(kSym).raw_position; }
};

// A long fall: every speed is negative.
std::vector<Bar> falling() { return path(700, 0, 0.0, 0.0012); }
// A long rise and then a sharp fall of thirty bars: the fast speeds are negative and strong, the
// combined forecast is negative, and the slowest speed (64,256) is still positive.
std::vector<Bar> rise_then_drop() { return path(700, 670, 0.0015, 0.0080); }
// A long rise: every speed is positive.
std::vector<Bar> rising() { return path(700, 700, 0.0012, 0.0); }

}  // namespace

// Both slow speeds negative: the equity short stands, equal to the unruled forecast.
TEST(EquitySlowRule, AShortWithBothSlowSpeedsNegativeStands) {
    StateManager::reset_instance();
    const auto bars = falling();
    Sleeve plain("SLOW_stands_plain", kSix, {});
    Sleeve ruled("SLOW_stands_ruled", kSix, {kSym});
    const double unruled = plain.forecast(bars);
    ASSERT_LT(unruled, -1.0) << "the fixture is not a short";
    EXPECT_EQ(ruled.forecast(bars), unruled);
    EXPECT_LT(ruled.position(), 0.0);
}

// One slow speed positive: the negative combined forecast is set to 0, and so is the position.
TEST(EquitySlowRule, AShortWithOneSlowSpeedPositiveIsZeroed) {
    StateManager::reset_instance();
    const auto bars = rise_then_drop();
    Sleeve plain("SLOW_zero_plain", kSix, {});
    Sleeve ruled("SLOW_zero_ruled", kSix, {kSym});
    const double unruled = plain.forecast(bars);
    ASSERT_LT(unruled, -1.0) << "the fixture's combined forecast is not negative";
    // The fixture's slowest speed is positive: a sleeve of that pair alone has a positive forecast.
    Sleeve slowest("SLOW_zero_slowest", {{64, 256}}, {});
    ASSERT_GT(slowest.forecast(bars), 0.0) << "the fixture's (64,256) speed is not positive";
    EXPECT_EQ(ruled.forecast(bars), 0.0);
    EXPECT_EQ(ruled.position(), 0.0);
}

// A positive forecast is untouched.
TEST(EquitySlowRule, APositiveForecastIsUntouched) {
    StateManager::reset_instance();
    const auto bars = rising();
    Sleeve plain("SLOW_long_plain", kSix, {});
    Sleeve ruled("SLOW_long_ruled", kSix, {kSym});
    const double unruled = plain.forecast(bars);
    ASSERT_GT(unruled, 1.0);
    EXPECT_EQ(ruled.forecast(bars), unruled);
}

// The FAST sleeve carries neither slow pair and is not ruled: its equity short stands with no
// slow agreement behind it.
TEST(EquitySlowRule, AFastSleeveShortWithNoSlowAgreementIsUntouched) {
    StateManager::reset_instance();
    const auto bars = rise_then_drop();
    Sleeve fast("SLOW_fast", kFast, {});
    const double forecast = fast.forecast(bars);
    EXPECT_LT(forecast, -1.0) << "the fast sleeve's short stands";
    EXPECT_LT(fast.position(), 0.0);
}

// A symbol the rule does not name is untouched, whatever its slow speeds say.
TEST(EquitySlowRule, ASymbolTheRuleDoesNotNameIsUntouched) {
    StateManager::reset_instance();
    const auto bars = rise_then_drop();
    Sleeve plain("SLOW_other_plain", kSix, {});
    Sleeve ruled("SLOW_other_ruled", kSix, {"MES", "MNQ", "MYM", "M2K"});
    const double unruled = plain.forecast(bars);
    ASSERT_LT(unruled, -1.0);
    EXPECT_EQ(ruled.forecast(bars), unruled);
}

// A ruled sleeve carries the pairs the rule reads: the rule on a sleeve without them (the FAST
// sleeve put first in a book) is refused when the sleeve is built.
TEST(EquitySlowRule, ARuledSleeveWithoutTheSlowPairsIsRefused) {
    StateManager::reset_instance();
    Sleeve fast("SLOW_refused", kFast, {kSym}, kSlowPairs, /*start=*/false);
    ASSERT_TRUE(fast.initialized.is_error());
    EXPECT_NE(std::string(fast.initialized.error()->what()).find("(32, 128)"), std::string::npos);
}

// Both futures books' templates carry the rule as LOOP_SPEC section 7.5.1 writes it.
TEST(EquitySlowRule, TheFuturesTemplatesCarryTheRule) {
    std::filesystem::path root = std::filesystem::current_path();
    while (!(std::filesystem::exists(root / "CMakeLists.txt") &&
             std::filesystem::exists(root / "config_template"))) {
        ASSERT_NE(root, root.parent_path()) << "repository root not found";
        root = root.parent_path();
    }
    for (const char* book : {"conservative", "base"}) {
        std::ifstream in(root / "config_template" / "portfolios" / book / "portfolio.json");
        ASSERT_TRUE(in.good()) << book;
        const nlohmann::json portfolio = nlohmann::json::parse(in);
        ASSERT_TRUE(portfolio.contains("equity_slow_rule")) << book;
        EXPECT_EQ(portfolio["equity_slow_rule"]["symbols"],
                  nlohmann::json({"M2K", "MES", "MNQ", "MYM"}))
            << book;
        EXPECT_EQ(portfolio["equity_slow_rule"]["pairs"],
                  nlohmann::json::array({{32, 128}, {64, 256}}))
            << book;
    }
}
