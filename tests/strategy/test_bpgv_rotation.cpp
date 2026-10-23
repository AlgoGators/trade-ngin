// tests/strategy/test_bpgv_rotation.cpp
//
// BPGVRotationStrategy: config, validation, the daily cycle, the tolerance band and cash cap,
// and each step of the monthly weight pipeline. The macro data is the checked-in
// tests/fixtures/macro/bpgv_regime_fixture.csv (synthetic, 2022-01..2025-12).
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"

#define private public
#include "trade_ngin/strategy/bpgv_rotation.hpp"
#undef private

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::vector<std::string> kUniverse = {"SPY", "QQQ", "XLK", "SMH", "IWM", "XHB",
                                            "IYR", "EQR", "TLT", "GLD", "BIL", "DBMF"};

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

BPGVRotationConfig make_config() {
    BPGVRotationConfig cfg;
    cfg.macro_csv_path = find_repo_file("tests/fixtures/macro/bpgv_regime_fixture.csv").string();
    return cfg;
}

StrategyConfig make_strategy_config() {
    StrategyConfig sc;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.capital_allocation = 100000.0;
    sc.max_leverage = 1.0;
    sc.max_drawdown = 0.5;
    for (const auto& sym : kUniverse) {
        sc.trading_params[sym] = 1.0;
        sc.position_limits[sym] = 10000.0;
    }
    return sc;
}

std::unique_ptr<BPGVRotationStrategy> make_running_strategy(
    const std::string& id, std::shared_ptr<MockPostgresDatabase> db,
    BPGVRotationConfig cfg = make_config()) {
    auto strategy = std::make_unique<BPGVRotationStrategy>(id, make_strategy_config(), cfg, db);
    EXPECT_TRUE(strategy->initialize().is_ok());
    EXPECT_TRUE(strategy->start().is_ok());
    return strategy;
}

Timestamp utc_day(int y, int m, int d) {
    Timestamp out;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    EXPECT_TRUE(core::parse_utc_date(buf, out));
    return out;
}

std::vector<Bar> universe_bars(const Timestamp& ts, double base_price) {
    std::vector<Bar> bars;
    int i = 0;
    for (const auto& sym : kUniverse) {
        const double px = base_price + 3.0 * i++;
        bars.emplace_back(ts, Price(px), Price(px * 1.01), Price(px * 0.99), Price(px), 1e6, sym);
    }
    return bars;
}

// The last `sessions` weekdays ending on `end` (inclusive), oldest first.
std::vector<Timestamp> sessions_ending(const Timestamp& end, int sessions) {
    std::vector<Timestamp> out;
    for (Timestamp ts = end; static_cast<int>(out.size()) < sessions; ts -= std::chrono::hours(24)) {
        auto tt = std::chrono::system_clock::to_time_t(ts);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        if (tm.tm_wday != 0 && tm.tm_wday != 6) out.push_back(ts);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// A deterministic, symbol-specific path: drift plus two slow cycles, so momentum, breakout
// and volatility filters all have something to decide on.
std::vector<Bar> path_bars(const Timestamp& ts, int day_index) {
    std::vector<Bar> bars;
    int i = 0;
    for (const auto& sym : kUniverse) {
        const double phase = 0.7 * i;
        const double px = (100.0 + 5.0 * i) *
                          (1.0 + 0.00025 * day_index + 0.06 * std::sin(day_index / 41.0 + phase) +
                           0.03 * std::cos(day_index / 97.0 - phase));
        bars.emplace_back(ts, Price(px), Price(px * 1.008), Price(px * 0.992), Price(px), 1e6, sym);
        ++i;
    }
    return bars;
}

std::deque<double> ramp(int n, double start, double daily_return) {
    std::deque<double> p{start};
    while (static_cast<int>(p.size()) < n) p.push_back(p.back() * (1.0 + daily_return));
    return p;
}

// Returns alternating +amplitude / -amplitude: a realized volatility of about amplitude x sqrt(252).
std::deque<double> zigzag(int n, double start, double amplitude) {
    std::deque<double> p{start};
    for (int i = 1; i < n; ++i) p.push_back(p.back() * (i % 2 ? 1.0 + amplitude : 1.0 - amplitude));
    return p;
}

// A rising year, then 21 volatile sessions that lose about 12% and close below the 200-day SMA.
std::deque<double> stressed_spy() {
    auto p = ramp(230, 100.0, 0.0005);
    for (int i = 0; i < 21; ++i) p.push_back(p.back() * (0.995 + (i % 2 ? 0.02 : -0.02)));
    return p;
}

void set_history(BPGVRotationStrategy& s, const std::string& sym, const std::deque<double>& close) {
    auto& state = s.symbol_state_.at(sym);
    state.price_history = close;
    state.high_history.clear();
    state.low_history.clear();
    for (double c : close) {
        state.high_history.push_back(c * 1.01);
        state.low_history.push_back(c * 0.99);
    }
    state.current_price = close.back();
}

// A buy fill, the way the coordinator feeds executions back to the strategy.
void fill(BPGVRotationStrategy& s, const std::string& sym, double qty, double price) {
    ExecutionReport exec;
    exec.order_id = "T-" + sym;
    exec.exec_id = "E-" + sym;
    exec.symbol = sym;
    exec.side = Side::BUY;
    exec.filled_quantity = Quantity(qty);
    exec.fill_price = Price(price);
    exec.fill_time = std::chrono::system_clock::now();
    ASSERT_TRUE(s.on_execution(exec).is_ok()) << sym;
}

// Annualised sample stdev of the last `window` simple returns: an independent reference for the
// strategy's volatility estimates.
double annualised_sd(const std::deque<double>& prices, int window) {
    std::vector<double> r;
    for (size_t i = prices.size() - window; i < prices.size(); ++i) {
        r.push_back(prices[i] / prices[i - 1] - 1.0);
    }
    double mean = 0.0;
    for (double x : r) mean += x;
    mean /= r.size();
    double var = 0.0;
    for (double x : r) var += (x - mean) * (x - mean);
    return std::sqrt(var / (r.size() - 1)) * std::sqrt(252.0);
}

double total(const std::unordered_map<std::string, double>& w) {
    double t = 0.0;
    for (const auto& [sym, x] : w) t += x;
    return t;
}

}  // namespace

class BPGVRotationTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        if (make_config().macro_csv_path.empty()) {
            GTEST_SKIP() << "tests/fixtures/macro not reachable from the test working directory";
        }
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        if (db_) db_->disconnect();
        TestBase::TearDown();
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

// ===========================================================================
// Config and template
// ===========================================================================

TEST(BPGVRotationConfig, FromJsonReadsEveryBlockAndKeepsDefaultsForAbsentKeys) {
    const nlohmann::json j = {
        {"macro_csv_path", "x/macro.csv"},
        {"rebalance_day_of_month", 22},
        {"allow_fractional_shares", false},
        {"base_risk_off_range", 0.30},
        {"risk_on_symbols", {"SPY", "QQQ"}},
        {"cash_symbols", {"BIL"}},
        {"rebalance", {{"drift_abs_trigger", 0.025}}},
        {"momentum", {{"tsm_tolerance", 0.05}}},
        {"breakout", {{"sma_window", 150}}},
        {"vol_target", {{"enabled", true}, {"stress_vol_window_days", 10}}},
        {"defensive_sleeve", {{"garch_aware_enabled", true}}},
        {"market_stress", {{"drawdown_trigger", -0.12}}},
        {"breadth_risk", {{"symbols", {"SPY", "IWM"}}}},
        {"bpg_growth_signal", {{"lookback_months", 6}}},
        {"housing_exposure", {{"cap_scale", 0.5}}},
        {"duration_defense", {{"duration_symbol", "IEF"}}},
    };
    const auto cfg = BPGVRotationConfig::from_json(j);
    EXPECT_EQ(cfg.macro_csv_path, "x/macro.csv");
    EXPECT_EQ(cfg.rebalance_day_of_month, 22);
    EXPECT_FALSE(cfg.allow_fractional_shares);
    EXPECT_DOUBLE_EQ(cfg.base_risk_off_range, 0.30);
    EXPECT_EQ(cfg.risk_on_symbols, (std::vector<std::string>{"SPY", "QQQ"}));
    EXPECT_EQ(cfg.cash_symbols, (std::vector<std::string>{"BIL"}));
    EXPECT_DOUBLE_EQ(cfg.rebalance.drift_abs_trigger, 0.025);
    EXPECT_DOUBLE_EQ(cfg.momentum.tsm_tolerance, 0.05);
    EXPECT_EQ(cfg.breakout.sma_window, 150);
    EXPECT_TRUE(cfg.vol_target.enabled);
    EXPECT_EQ(cfg.vol_target.stress_vol_window_days, 10);
    EXPECT_TRUE(cfg.defensive_sleeve.garch_aware_enabled);
    EXPECT_DOUBLE_EQ(cfg.market_stress.drawdown_trigger, -0.12);
    EXPECT_EQ(cfg.breadth_risk.symbols, (std::vector<std::string>{"SPY", "IWM"}));
    EXPECT_EQ(cfg.bpg_growth_signal.lookback_months, 6);
    EXPECT_DOUBLE_EQ(cfg.housing_exposure.cap_scale, 0.5);
    EXPECT_EQ(cfg.duration_defense.duration_symbol, "IEF");

    const BPGVRotationConfig defaults;
    EXPECT_EQ(cfg.risk_off_symbols, defaults.risk_off_symbols);
    EXPECT_DOUBLE_EQ(cfg.rebalance.drift_rel_trigger, defaults.rebalance.drift_rel_trigger);
    EXPECT_EQ(cfg.momentum.tsm_lookback_days, defaults.momentum.tsm_lookback_days);

    const auto out = cfg.to_json();
    EXPECT_EQ(out["rebalance_day_of_month"], 22);
    EXPECT_EQ(out["as_configured"]["vol_target"]["stress_vol_window_days"], 10);
    EXPECT_EQ(BPGVRotationConfig::from_json(nlohmann::json::object()).to_json()["as_configured"],
              nlohmann::json::object());
}

TEST(BPGVRotationConfig, RequiredWarmupIsTheLongestFilterWindow) {
    BPGVRotationConfig cfg;
    EXPECT_EQ(cfg.required_warmup_days(), 252) << "the 12-month momentum gate";
    cfg.breadth_risk.enabled = true;
    EXPECT_EQ(cfg.required_warmup_days(), 253) << "breadth's 252-day return needs 253 bars";
    cfg.breakout.sma_window = 300;
    EXPECT_EQ(cfg.required_warmup_days(), 300);
}

// The shipped template is what a fresh deploy copies, and nothing else in the suite loads it:
// it must pass the schema-2 ConfigLoader, run the `none` assignment ruled for this book, keep
// the old limits for measurement only, and pass the equity runners' optimizer guard.
TEST(BPGVTemplate, LoadsUnderTheSchema2LoaderWithTheRuledNoneAssignment) {
    const auto defaults = find_repo_file("config_template/defaults.json");
    if (defaults.empty()) GTEST_SKIP() << "config_template/ not reachable from the test directory";
    auto loaded = ConfigLoader::load(defaults.parent_path(), "bpgv_rotation");
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    const AppConfig& cfg = loaded.value();
    EXPECT_EQ(cfg.portfolio_id, "BPGV_ROTATION_PORTFOLIO");

    EXPECT_FALSE(cfg.use_optimization);
    EXPECT_TRUE(apps::refuse_if_optimizer_requested(cfg.use_optimization).is_ok());

    ASSERT_EQ(cfg.risk_schema.portfolio.size(), 1u);
    EXPECT_EQ(cfg.risk_schema.portfolio.front().type, "none");
    EXPECT_TRUE(cfg.risk_schema.sleeves.empty());

    // Still measured: the reporter keeps the limits the schema-1 gate used.
    EXPECT_DOUBLE_EQ(cfg.risk_config.var_limit, 0.25);
    EXPECT_DOUBLE_EQ(cfg.risk_config.jump_risk_limit, 0.08);

    EXPECT_TRUE(apps::refuse_if_nav_sized_book_has_a_capital_gate(cfg.risk_schema).is_ok());
}

// The parser ignores unknown keys, so a retired setting left in the template would look live
// and change nothing.
TEST(BPGVTemplate, CarriesNoRetiredKey) {
    const auto path = find_repo_file("config_template/portfolios/bpgv_rotation/portfolio.json");
    if (path.empty()) GTEST_SKIP() << "config_template/ not reachable from the test directory";
    std::ifstream in(path);
    const auto doc = nlohmann::json::parse(in);
    const nlohmann::json& cfg = doc.at("strategies").at("BPGV_ROTATION").at("config");
    for (const char* key : {"crash_override", "crash_threshold", "crash_lookback_days",
                            "credit_stress", "momentum_tilt_scale", "momentum_lookback_days",
                            "breakout_sma_window"}) {
        EXPECT_FALSE(cfg.contains(key)) << key;
    }
    for (const char* key : {"mode", "index_gate_enabled"}) {
        EXPECT_FALSE(cfg.at("breakout").contains(key)) << "breakout." << key;
    }
    const nlohmann::json& vol_target = cfg.at("vol_target");
    for (const auto& [key, value] : vol_target.items()) {
        EXPECT_NE(key.rfind("realized_vol_", 0), 0u) << "vol_target." << key;
    }
}

TEST(BPGVTemplate, ANavSizedBookRefusesACarverGateOnThePortfolioOrASleeve) {
    // BPGV sizes on the prior-close NAV and the equity paths never move a module's capital
    // with it, so only a module that reads capital -- `carver` -- is refused.
    auto module = [](const std::string& type) {
        RiskModuleConfig m;
        m.id = type + "_module";
        m.type = type;
        return m;
    };
    RiskSchema none_only;
    none_only.portfolio = {module("none")};
    EXPECT_TRUE(apps::refuse_if_nav_sized_book_has_a_capital_gate(none_only).is_ok());

    RiskSchema capital_free;
    capital_free.portfolio = {module("constant_scale"), module("warn"), module("refuse")};
    EXPECT_TRUE(apps::refuse_if_nav_sized_book_has_a_capital_gate(capital_free).is_ok());

    RiskSchema carver_portfolio;
    carver_portfolio.portfolio = {module("warn"), module("carver")};
    auto refused = apps::refuse_if_nav_sized_book_has_a_capital_gate(carver_portfolio);
    ASSERT_TRUE(refused.is_error());
    EXPECT_EQ(refused.error()->code(), ErrorCode::INVALID_DATA);

    RiskSchema carver_sleeve;
    carver_sleeve.portfolio = {module("none")};
    carver_sleeve.sleeves["BPGV_ROTATION"] = {module("carver")};
    EXPECT_TRUE(apps::refuse_if_nav_sized_book_has_a_capital_gate(carver_sleeve).is_error());
}

// ===========================================================================
// Initialization and validation
// ===========================================================================

TEST_F(BPGVRotationTest, InitializesEveryUniverseSymbolIncludingTheCashSleeve) {
    auto s = make_running_strategy("INIT", db_);
    EXPECT_EQ(s->macro_data_.size(), 48u);
    EXPECT_EQ(s->required_warmup_days(), make_config().required_warmup_days());
    const auto history = s->get_price_history();
    const auto targets = s->get_target_positions();
    for (const auto& sym : kUniverse) {
        EXPECT_TRUE(history.count(sym)) << sym;
        ASSERT_TRUE(targets.count(sym)) << sym << ": a row for every symbol, so a dropped name is closed";
        EXPECT_DOUBLE_EQ(targets.at(sym).quantity.as_double(), 0.0) << sym;
    }
}

TEST_F(BPGVRotationTest, InvalidSettingsAreRefusedNamingTheSetting) {
    const std::vector<std::pair<std::function<void(BPGVRotationConfig&)>, std::string>> cases = {
        {[](BPGVRotationConfig& c) { c.rebalance_day_of_month = 29; }, "Rebalance day"},
        {[](BPGVRotationConfig& c) { c.risk_on_symbols.clear(); }, "Risk-on symbols"},
        {[](BPGVRotationConfig& c) { c.risk_off_symbols.clear(); }, "Risk-off symbols"},
        {[](BPGVRotationConfig& c) {
             c.market_stress.enabled = true;
             c.market_stress.risk_on_scale = 1.5;
         },
         "market_stress.risk_on_scale"},
        {[](BPGVRotationConfig& c) {
             c.market_stress.enabled = true;
             c.market_stress.drawdown_trigger = 0.0;
         },
         "market_stress.drawdown_trigger"},
        {[](BPGVRotationConfig& c) {
             c.breadth_risk.enabled = true;
             c.breadth_risk.full_risk_breadth = 0.0;
         },
         "breadth_risk"},
        {[](BPGVRotationConfig& c) {
             c.bpg_growth_signal.enabled = true;
             c.bpg_growth_signal.growth_scale = 0.0;
         },
         "bpg_growth_signal"},
        {[](BPGVRotationConfig& c) {
             c.housing_exposure.enabled = true;
             c.housing_exposure.cap_scale = 1.5;
         },
         "housing_exposure.cap_scale"},
        {[](BPGVRotationConfig& c) {
             c.duration_defense.enabled = true;
             c.duration_defense.guard_scale = -0.1;
         },
         "duration_defense.guard_scale"},
        {[](BPGVRotationConfig& c) {
             c.vol_target.enabled = true;
             c.vol_target.use_max_risk_sigma = true;
             c.vol_target.stress_vol_window_days = 1;
         },
         "vol_target max-risk windows"},
    };
    for (const auto& [mutate, expected] : cases) {
        auto cfg = make_config();
        mutate(cfg);
        BPGVRotationStrategy s("INVALID", make_strategy_config(), cfg, db_);
        auto r = s.initialize();
        ASSERT_TRUE(r.is_error()) << expected;
        EXPECT_NE(std::string(r.error()->what()).find(expected), std::string::npos)
            << r.error()->what();
    }
}

TEST_F(BPGVRotationTest, InitializationFailsWithoutItsMacroData) {
    auto cfg = make_config();
    cfg.macro_csv_path = "no/such/macro.csv";
    BPGVRotationStrategy s("NO_MACRO", make_strategy_config(), cfg, db_);
    auto r = s.initialize();
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::NOT_INITIALIZED);
}

// ===========================================================================
// Daily cycle
// ===========================================================================

TEST_F(BPGVRotationTest, OnDataFillsHistoriesWithoutPreload) {
    auto s = make_running_strategy("NO_PRELOAD", db_);
    for (const auto& [sym, hist] : s->get_price_history()) {
        EXPECT_TRUE(hist.empty()) << sym << " must start empty: there is no private pre-load";
    }
    for (int d = 2; d <= 12; ++d) {
        ASSERT_TRUE(s->on_data(universe_bars(utc_day(2024, 1, d), 100.0 + d)).is_ok());
    }
    const auto history = s->get_price_history();
    EXPECT_EQ(history.at("QQQ").size(), 11u);
    EXPECT_EQ(history.at("BIL").size(), 11u);
}

TEST_F(BPGVRotationTest, PriceHistoryIsCappedAtMaxPriceHistory) {
    auto s = make_running_strategy("TRIM", db_);
    Timestamp ts = utc_day(2022, 1, 3);
    for (int i = 0; i < 530; ++i, ts += std::chrono::hours(24)) {
        ASSERT_TRUE(s->on_data(universe_bars(ts, 100.0 + 0.01 * i)).is_ok());
    }
    EXPECT_EQ(s->get_price_history().at("SPY").size(), BPGVRotationStrategy::MAX_PRICE_HISTORY);
}

TEST_F(BPGVRotationTest, OnDataProcessesMultiDayVectorPerDay) {
    // A: one call per day. B: both days in one vector. Same state afterwards, including the
    // day-22 rebalance decision.
    auto a = make_running_strategy("PER_DAY", db_);
    auto b = make_running_strategy("GROUPED", db_);
    auto day1 = universe_bars(utc_day(2024, 1, 19), 100.0);
    auto day2 = universe_bars(utc_day(2024, 1, 22), 101.0);
    ASSERT_TRUE(a->on_data(day1).is_ok());
    ASSERT_TRUE(a->on_data(day2).is_ok());
    std::vector<Bar> both = day1;
    both.insert(both.end(), day2.begin(), day2.end());
    ASSERT_TRUE(b->on_data(both).is_ok());

    const auto ha = a->get_price_history();
    const auto hb = b->get_price_history();
    for (const auto& [sym, hist] : ha) {
        ASSERT_EQ(hist.size(), 2u) << sym;
        ASSERT_EQ(hb.at(sym), hist) << sym;
    }
    const auto ta = a->get_target_positions();
    const auto tb = b->get_target_positions();
    ASSERT_EQ(ta.size(), 12u);
    for (const auto& [sym, pos] : ta) {
        EXPECT_DOUBLE_EQ(pos.quantity.as_double(), tb.at(sym).quantity.as_double()) << sym;
    }
}

TEST_F(BPGVRotationTest, AMonthRebalancesOnceOnItsFirstEligibleDay) {
    auto cfg = make_config();
    auto a = make_running_strategy("ONCE_A", db_, cfg);
    auto b = make_running_strategy("ONCE_B", db_, cfg);
    const int rd = cfg.rebalance_day_of_month;
    std::vector<Bar> grouped;
    std::unordered_map<std::string, double> before, on_day, next_day;
    for (int d = rd - 3; d <= rd + 2; ++d) {
        auto bars = universe_bars(utc_day(2024, 1, d), 100.0 + d);
        ASSERT_TRUE(a->on_data(bars).is_ok());
        grouped.insert(grouped.end(), bars.begin(), bars.end());
        if (d == rd - 1) before = a->get_current_weights();
        if (d == rd) on_day = a->get_current_weights();
        if (d == rd + 1) next_day = a->get_current_weights();
    }
    EXPECT_TRUE(before.empty()) << "no rebalance before the rebalance day";
    ASSERT_FALSE(on_day.empty()) << "the first session >= rebalance day rebalances";
    EXPECT_EQ(a->get_rebalance_count(), 1) << "three sessions on or after the day, one rebalance";
    for (const auto& [sym, w] : on_day) EXPECT_NEAR(w, next_day.at(sym), 1e-12) << sym;

    ASSERT_TRUE(b->on_data(grouped).is_ok());
    EXPECT_EQ(b->get_rebalance_count(), 1) << "the grouped feed also rebalances once";
    for (const auto& [sym, w] : a->get_current_weights()) {
        EXPECT_NEAR(w, b->get_current_weights().at(sym), 1e-12) << sym;
    }
}

TEST_F(BPGVRotationTest, TwoStartOffsetsConvergeToTheSameWeights) {
    // Two windows ending on the same day, one 60 sessions longer. Both exceed the longest
    // lookback, so the weights must not depend on where the history starts.
    const auto cfg = make_config();
    const int short_window = cfg.required_warmup_days() + 120;
    const int long_window = short_window + 60;
    const auto days = sessions_ending(utc_day(2025, 6, 30), long_window);

    auto a = make_running_strategy("REPLAY_LONG", db_, cfg);
    auto b = make_running_strategy("REPLAY_SHORT", db_, cfg);
    for (int i = 0; i < long_window; ++i) {
        ASSERT_TRUE(a->on_data(path_bars(days[i], i)).is_ok());
        if (i >= long_window - short_window) ASSERT_TRUE(b->on_data(path_bars(days[i], i)).is_ok());
    }
    const auto& wa = a->get_current_weights();
    const auto& wb = b->get_current_weights();
    ASSERT_FALSE(wa.empty()) << "a window this long must have rebalanced";
    ASSERT_EQ(wa.size(), wb.size());
    for (const auto& [sym, w] : wa) EXPECT_NEAR(w, wb.at(sym), 1e-12) << sym;
    const auto ta = a->get_target_positions();
    const auto tb = b->get_target_positions();
    for (const auto& [sym, pos] : ta) {
        EXPECT_NEAR(pos.quantity.as_double(), tb.at(sym).quantity.as_double(), 1e-9) << sym;
    }
}

TEST_F(BPGVRotationTest, RebalanceWeightsAreLongOnlyAndSumToAtMostOne) {
    auto s = make_running_strategy("WEIGHTS", db_);
    ASSERT_TRUE(s->on_data(universe_bars(utc_day(2024, 1, 22), 100.0)).is_ok());
    const auto& weights = s->get_current_weights();
    for (const auto& [sym, w] : weights) EXPECT_GE(w, -1e-12) << sym << ": weights are long-only";
    EXPECT_GT(total(weights), 0.0) << "a rebalance day sets weights";
    EXPECT_LE(total(weights), 1.0 + 1e-9) << "sum(weights) <= 1, the residual is cash";
}

TEST(BPGVExtractDate, IsUtcRegardlessOfHostZone) {
    const char* old_tz = std::getenv("TZ");
    setenv("TZ", "America/New_York", 1);
    tzset();
    auto parts = BPGVRotationStrategy::extract_date(utc_day(2024, 1, 31));
    if (old_tz) setenv("TZ", old_tz, 1); else unsetenv("TZ");
    tzset();
    EXPECT_EQ(parts.year, 2024);
    EXPECT_EQ(parts.month, 1);
    EXPECT_EQ(parts.day, 31) << "localtime would have said the 30th on a New York host";
}

// ===========================================================================
// Sizing: NAV, the tolerance band, whole shares and the cash cap
// ===========================================================================

TEST_F(BPGVRotationTest, OnPortfolioValueSizesTargets) {
    auto base = make_running_strategy("NAV_BASE", db_);
    auto doubled = make_running_strategy("NAV_2X", db_);
    doubled->on_portfolio_value(2.0 * make_strategy_config().capital_allocation);
    const auto bars = universe_bars(utc_day(2024, 1, 22), 100.0);
    ASSERT_TRUE(base->on_data(bars).is_ok());
    ASSERT_TRUE(doubled->on_data(bars).is_ok());
    const auto tb = base->get_target_positions();
    const auto td = doubled->get_target_positions();
    int sized = 0;
    for (const auto& [sym, pos] : tb) {
        if (pos.quantity.as_double() <= 0.0) continue;
        ++sized;
        EXPECT_NEAR(td.at(sym).quantity.as_double(), 2.0 * pos.quantity.as_double(), 1e-4) << sym;
    }
    EXPECT_GT(sized, 0) << "the rebalance must have produced at least one non-zero target";
}

TEST_F(BPGVRotationTest, NoTradeInsideBandSnapsToHeldQuantity) {
    auto s = make_running_strategy("BAND_SNAP", db_);
    ASSERT_TRUE(s->on_data(universe_bars(utc_day(2024, 1, 22), 100.0)).is_ok());
    int filled = 0;
    for (const auto& [sym, pos] : s->get_target_positions()) {
        if (pos.quantity.as_double() <= 0.0) continue;
        ExecutionReport exec;
        exec.order_id = "T-" + sym;
        exec.exec_id = "E-" + sym;
        exec.symbol = sym;
        exec.side = Side::BUY;
        exec.filled_quantity = pos.quantity;
        exec.fill_price = Price(100.0);
        exec.fill_time = utc_day(2024, 1, 22);
        ASSERT_TRUE(s->on_execution(exec).is_ok()) << sym;
        ++filled;
    }
    ASSERT_GT(filled, 0);

    // Inside the band the target IS the held quantity, never re-derived from the weight.
    const auto again = s->get_target_positions();
    for (const auto& [sym, pos] : s->get_positions()) {
        if (pos.quantity.as_double() > 0.0) EXPECT_EQ(again.at(sym).quantity, pos.quantity) << sym;
    }
    // A 0.1% price move next day stays inside the band.
    ASSERT_TRUE(s->on_data(universe_bars(utc_day(2024, 1, 23), 100.1)).is_ok());
    const auto next = s->get_target_positions();
    for (const auto& [sym, pos] : s->get_positions()) {
        if (pos.quantity.as_double() > 0.0) EXPECT_EQ(next.at(sym).quantity, pos.quantity) << sym;
    }
}

TEST_F(BPGVRotationTest, TheBandTradesHalfwayBackAndExitsOrEntersInFull) {
    auto s = make_running_strategy("BAND", db_);
    for (const char* sym : {"SPY", "QQQ", "XLK"}) set_history(*s, sym, {100.0});
    s->current_weights_ = {{"SPY", 0.10}, {"QQQ", 0.0}, {"XLK", 0.05}};
    fill(*s, "SPY", 200.0, 100.0);  // 20% of 100k against a 10% target
    fill(*s, "QQQ", 50.0, 100.0);   // held, target 0
    const auto t = s->get_target_positions();
    EXPECT_NEAR(t.at("SPY").quantity.as_double(), 150.0, 1e-9) << "halfway: 20% -> 15%";
    EXPECT_DOUBLE_EQ(t.at("QQQ").quantity.as_double(), 0.0) << "a zero target exits in full";
    EXPECT_NEAR(t.at("XLK").quantity.as_double(), 50.0, 1e-9) << "a new name enters in full";
}

TEST_F(BPGVRotationTest, WholeSharesModeFloorsEachTarget) {
    auto cfg = make_config();
    cfg.allow_fractional_shares = false;
    auto s = make_running_strategy("WHOLE", db_, cfg);
    set_history(*s, "SPY", {300.0});
    s->current_weights_ = {{"SPY", 0.10}};
    EXPECT_DOUBLE_EQ(s->get_target_positions().at("SPY").quantity.as_double(), 33.0)
        << "10,000 / 300 = 33.3 shares";
}

TEST_F(BPGVRotationTest, TheCashCapFundsEntriesFromWhatNavLeavesAndKeepsTrimmedTargets) {
    auto s = make_running_strategy("CASH_CAP", db_);
    for (const char* sym : {"SPY", "QQQ", "XLK", "SMH"}) set_history(*s, sym, {100.0});
    // held {SPY .5, QQQ .4} -> band targets {SPY .4, QQQ .325 (halfway), XLK .4, SMH .4}
    fill(*s, "SPY", 500.0, 100.0);
    fill(*s, "QQQ", 400.0, 100.0);
    s->current_weights_ = {{"SPY", 0.3}, {"QQQ", 0.25}, {"XLK", 0.4}, {"SMH", 0.4}};
    const auto t = s->get_target_positions();
    EXPECT_NEAR(t.at("SPY").quantity.as_double(), 400.0, 1e-9) << "a trimmed leg keeps its target";
    EXPECT_NEAR(t.at("QQQ").quantity.as_double(), 325.0, 1e-9);
    EXPECT_NEAR(t.at("XLK").quantity.as_double(), 137.5, 1e-6) << "the entries share the 27.5k left";
    EXPECT_NEAR(t.at("SMH").quantity.as_double(), 137.5, 1e-6);
    double book = 0.0;
    for (const auto& [sym, pos] : t) book += pos.quantity.as_double() * s->symbol_state_.at(sym).current_price;
    EXPECT_LE(book, 100000.0 + 1e-6) << "the capped book never exceeds NAV";
}

// The band rule trims a held name only halfway while a new name enters in full, so a
// rebalance can ask for more than NAV; exposure_increment_scale funds the adding legs from the
// cash the book has. Notionals below are fractions of a NAV of 1.
TEST(BPGVExposureCap, TheReviewScenarioIsCappedAtNavWithoutTouchingTheTrimmedLegs) {
    const std::vector<ExposureLeg> legs{{0.5, 0.3}, {0.4, 0.25}, {0.0, 0.4}, {0.0, 0.4}};
    const double s = exposure_increment_scale(legs, 1.0);
    EXPECT_NEAR(s, 0.45 / 0.8, 1e-12);
    double book = 0.0;
    for (const auto& leg : legs) {
        book += leg.target_notional > leg.held_notional
                    ? leg.held_notional + s * (leg.target_notional - leg.held_notional)
                    : leg.target_notional;
    }
    EXPECT_NEAR(book, 1.0, 1e-12) << "the capped book is exactly fully invested";
}

TEST(BPGVExposureCap, EverythingThatFitsIsLeftAlone) {
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.5, 0.3}, {0.4, 0.25}, {0.0, 0.2}, {0.0, 0.2}}, 1.0),
                     1.0);
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.6, 0.6}, {0.0, 0.4}}, 1.0), 1.0)
        << "an exact fit is not scaled";
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.5, 0.2}, {0.4, 0.0}}, 1.0), 1.0)
        << "a rebalance that only reduces adds nothing to scale";
    EXPECT_DOUBLE_EQ(exposure_increment_scale({}, 1.0), 1.0);
}

TEST(BPGVExposureCap, AnIncreaseIsScaledOnItsIncrementOnly) {
    // held .2 -> .6 next to an in-band .7: only .1 of cash is left, so the increase gets a
    // quarter of its .4 increment and never falls below what is already held.
    const double s = exposure_increment_scale({{0.2, 0.6}, {0.7, 0.7}}, 1.0);
    EXPECT_NEAR(s, 0.25, 1e-12);
    EXPECT_GE(0.2 + s * 0.4, 0.2);
}

TEST(BPGVExposureCap, NothingIsAddedWhenTheBookAlreadyReachesNavOrInputsAreUnusable) {
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{1.1, 1.1}, {0.0, 0.1}}, 1.0), 0.0)
        << "holdings already at or above NAV leave no cash for an entry";
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.0, 0.4}}, 0.0), 0.0) << "no NAV, no entries";
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.0, 0.4}}, -5.0), 0.0);
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.0, 0.4}}, std::nan("")), 0.0);
    EXPECT_DOUBLE_EQ(exposure_increment_scale({{0.0, std::nan("")}}, 1.0), 0.0)
        << "an unusable leg adds nothing rather than an unknown amount";
}

// ===========================================================================
// The weight pipeline, step by step
// ===========================================================================

TEST_F(BPGVRotationTest, BaseWeightsMoveTheRiskOffShareWithTheRegimeScore) {
    auto s = make_running_strategy("BASE", db_);
    MonthlyMacroRecord rec;
    rec.regime_score = -1.0;
    const auto low = s->calculate_base_weights(rec);
    EXPECT_NEAR(total(low), 1.0, 1e-12);
    EXPECT_NEAR(low.at("TLT") + low.at("GLD"), 0.05 * 0.95 + 0.70 * 0.05, 1e-12)
        << "risk-off share 5%: 5% of the risk-on extreme plus 70% of the risk-off extreme";
    EXPECT_EQ(low.count("BIL"), 0u) << "the cash sleeve has no base weight";

    rec.regime_score = 1.0;
    const auto high = s->calculate_base_weights(rec);
    EXPECT_NEAR(high.at("TLT") + high.at("GLD"), 0.05 * 0.55 + 0.70 * 0.45, 1e-12) << "45%";
    EXPECT_DOUBLE_EQ(high.at("SPY"), high.at("EQR")) << "equal weight within a sleeve";

    rec.strong_risk_on = true;
    const auto strong = s->calculate_base_weights(rec);
    EXPECT_NEAR(strong.at("SPY"), high.at("SPY") * 1.12, 1e-12);
    EXPECT_NEAR(strong.at("TLT"), high.at("TLT") * 0.80, 1e-12);
}

TEST_F(BPGVRotationTest, PermitGrowthShiftsTheRiskOffShareWithinItsClip) {
    auto cfg = make_config();
    cfg.bpg_growth_signal.enabled = true;  // 3 months, scale 0.05, shift up to 0.10
    auto s = make_running_strategy("GROWTH", db_, cfg);
    s->macro_data_.assign(3, MonthlyMacroRecord{});
    for (int m = 0; m < 3; ++m) {
        s->macro_data_[m].year = 2024;
        s->macro_data_[m].month = m + 1;
        s->macro_data_[m].bpg_sfh = 0.10;
    }
    const auto rec = s->macro_data_.back();  // regime 0: a 25% base risk-off share
    EXPECT_DOUBLE_EQ(s->compute_bpg_growth_signal(rec), 1.0) << "0.10 / 0.05 clips to +1";
    const auto w = s->calculate_base_weights(rec);
    EXPECT_NEAR(w.at("TLT") + w.at("GLD"), 0.05 * 0.85 + 0.70 * 0.15, 1e-12) << "25% - 10% = 15%";

    for (auto& r : s->macro_data_) r.bpg_sfh = -0.01;
    EXPECT_NEAR(s->compute_bpg_growth_signal(s->macro_data_.back()), -0.2, 1e-12);
}

TEST_F(BPGVRotationTest, MomentumGateZerosALaggardAndTheTiltFavoursTheLeader) {
    auto cfg = make_config();
    cfg.risk_on_symbols = {"SPY", "QQQ", "XLK"};
    cfg.momentum.xsec_use_vol_scaling = false;  // rank on return alone
    auto s = make_running_strategy("MOMENTUM", db_, cfg);
    set_history(*s, "BIL", ramp(260, 100.0, 0.0));
    set_history(*s, "SPY", ramp(260, 100.0, 0.001));
    set_history(*s, "QQQ", ramp(260, 100.0, 0.0002));
    set_history(*s, "XLK", ramp(260, 100.0, -0.0002));  // trails the risk-free symbol
    std::unordered_map<std::string, double> w{{"SPY", 0.2}, {"QQQ", 0.2}, {"XLK", 0.2}, {"TLT", 0.4}};
    s->apply_momentum_tilt(w);
    EXPECT_DOUBLE_EQ(w.at("XLK"), 0.0) << "the 12-month gate zeroes a name below the risk-free return";
    EXPECT_NEAR(w.at("SPY"), 0.2 * 1.4, 1e-12) << "the leader is tilted up by tau";
    EXPECT_NEAR(w.at("QQQ"), 0.2 * 0.6, 1e-12) << "the laggard down by tau, above the floor";
    EXPECT_DOUBLE_EQ(w.at("TLT"), 0.4) << "the risk-off sleeve is untouched";
}

TEST(BPGVWilderATR, MatchesHandComputedValues) {
    std::deque<double> high, low, close;
    for (int i = 0; i < 30; ++i) {
        close.push_back(100.0 + i);
        high.push_back(101.0 + i);
        low.push_back(99.0 + i);
    }
    EXPECT_NEAR(BPGVRotationStrategy::wilder_atr(high, low, close, 14), 2.0, 1e-12)
        << "a constant true range of 2";
    EXPECT_NEAR(BPGVRotationStrategy::wilder_atr({100, 110}, {100, 105}, {100, 108}, 1), 10.0, 1e-12)
        << "a gap: the true range is |high - previous close|";
    EXPECT_DOUBLE_EQ(BPGVRotationStrategy::wilder_atr(high, low, close, 30), 0.0) << "too short";
}

TEST_F(BPGVRotationTest, BreakoutGradesANameAboveItsSmaAndZeroesOneConfirmedBelowIt) {
    auto s = make_running_strategy("BREAKOUT", db_);
    std::deque<double> close(201, 100.0), high(201, 101.0), low(201, 99.0);
    EXPECT_DOUBLE_EQ(s->breakout_score({101.0}, {99.0}, {100.0}), 1.0) << "short history fails open";

    // Last close 102 over a 200-day SMA of 100.01; the last true range is 3, so the Wilder ATR
    // is (13 x 2 + 3) / 14.
    close.back() = 102.0;
    high.back() = 103.0;
    low.back() = 101.0;
    EXPECT_NEAR(s->breakout_score(high, low, close), (102.0 - 100.01) / (2.0 * 29.0 / 14.0), 1e-12);

    for (int i = 198; i <= 200; ++i) {
        close[i] = 99.0;
        high[i] = 100.0;
        low[i] = 98.0;
    }
    EXPECT_DOUBLE_EQ(s->breakout_score(high, low, close), 0.0) << "three closes below the SMA";
}

TEST_F(BPGVRotationTest, TheDurationGuardHalvesTltOnlyWhileItsTrendIsPoor) {
    auto cfg = make_config();
    cfg.duration_defense.enabled = true;  // 126-day return, 200-day SMA, scale 0.5, splice BIL
    auto s = make_running_strategy("DURATION", db_, cfg);
    set_history(*s, "TLT", ramp(210, 100.0, -0.001));
    std::unordered_map<std::string, double> falling{{"TLT", 0.3}, {"BIL", 0.0}};
    s->apply_duration_defense_guard(falling);
    EXPECT_NEAR(falling.at("TLT"), 0.15, 1e-12);
    EXPECT_NEAR(falling.at("BIL"), 0.15, 1e-12);

    set_history(*s, "TLT", ramp(210, 100.0, 0.001));
    std::unordered_map<std::string, double> rising{{"TLT", 0.3}};
    s->apply_duration_defense_guard(rising);
    EXPECT_DOUBLE_EQ(rising.at("TLT"), 0.3);
}

TEST_F(BPGVRotationTest, TheHousingCapAppliesOnlyWhenGarchIsHighAndGrowthIsWeak) {
    auto cfg = make_config();
    cfg.housing_exposure.enabled = true;  // GARCH percentile >= 75, growth <= 0, cap 0.6
    auto s = make_running_strategy("HOUSING", db_, cfg);
    MonthlyMacroRecord rec;
    rec.bpgv_garch = 0.03;
    rec.bpgv_garch_percentile = 80.0;
    std::unordered_map<std::string, double> w{
        {"XHB", 0.1}, {"IYR", 0.1}, {"EQR", 0.1}, {"SPY", 0.2}, {"BIL", 0.0}};
    s->apply_housing_exposure_cap(w, rec);
    EXPECT_NEAR(w.at("XHB"), 0.06, 1e-12);
    EXPECT_NEAR(w.at("BIL"), 0.12, 1e-12) << "the three housing names release 0.04 each";
    EXPECT_DOUBLE_EQ(w.at("SPY"), 0.2);

    rec.bpgv_garch_percentile = 50.0;
    std::unordered_map<std::string, double> calm{{"XHB", 0.1}};
    s->apply_housing_exposure_cap(calm, rec);
    EXPECT_DOUBLE_EQ(calm.at("XHB"), 0.1);
}

TEST_F(BPGVRotationTest, NarrowBreadthScalesTheRiskOnSleeveAndSplicesToBil) {
    auto cfg = make_config();
    cfg.breadth_risk.enabled = true;
    cfg.breadth_risk.full_risk_breadth = 0.5;  // stress at <= 0.5, floor 0.25
    auto s = make_running_strategy("BREADTH", db_, cfg);
    set_history(*s, "BIL", ramp(260, 100.0, 0.0));
    std::unordered_map<std::string, double> w{{"BIL", 0.0}};
    for (size_t i = 0; i < cfg.risk_on_symbols.size(); ++i) {
        const auto& sym = cfg.risk_on_symbols[i];
        set_history(*s, sym, ramp(260, 100.0, i < 2 ? 0.001 : -0.001));  // 2 of 8 pass
        w[sym] = 0.1;
    }
    s->apply_breadth_risk_permission(w);
    EXPECT_NEAR(w.at("SPY"), 0.05, 1e-12) << "breadth 0.25 / full 0.5 = permission 0.5";
    EXPECT_NEAR(w.at("BIL"), 0.4, 1e-12);
    EXPECT_TRUE(s->is_breadth_stress_active());
}

TEST_F(BPGVRotationTest, TheDefensivePairLeansToTheCalmerLegAndSplicesExcessVolToBil) {
    auto cfg = make_config();
    cfg.defensive_sleeve.vol_aware_enabled = true;  // pair target 10%
    auto s = make_running_strategy("DEFENSIVE", db_, cfg);
    set_history(*s, "TLT", zigzag(80, 100.0, 0.02));  // about 32% annualised
    set_history(*s, "GLD", zigzag(80, 100.0, 0.01));  // about 16%
    std::unordered_map<std::string, double> w{{"TLT", 0.2}, {"GLD", 0.2}, {"BIL", 0.0}};
    s->apply_defensive_sleeve_vol_awareness(w, MonthlyMacroRecord{});
    EXPECT_GT(w.at("GLD"), w.at("TLT")) << "inverse-vol: the calmer leg gets more";
    EXPECT_GT(w.at("BIL"), 0.0) << "the pair's joint vol is above its target";
    EXPECT_NEAR(w.at("TLT") + w.at("GLD") + w.at("BIL"), 0.4, 1e-12) << "the splice only moves weight";
}

TEST_F(BPGVRotationTest, NormalizeRescalesToOneAndFallsBackToEqualWeight) {
    auto s = make_running_strategy("NORMALIZE", db_);
    std::unordered_map<std::string, double> w{{"SPY", 0.3}, {"TLT", 0.1}, {"QQQ", -0.2}};
    s->normalize_weights(w);
    EXPECT_NEAR(w.at("SPY"), 0.75, 1e-12);
    EXPECT_NEAR(w.at("TLT"), 0.25, 1e-12);
    EXPECT_DOUBLE_EQ(w.at("QQQ"), 0.0) << "a negative weight is dropped";

    std::unordered_map<std::string, double> zeroed{{"SPY", 0.0}};
    s->normalize_weights(zeroed);
    EXPECT_EQ(zeroed.size(), kUniverse.size());
    EXPECT_NEAR(zeroed.at("BIL"), 1.0 / 12.0, 1e-12) << "every filter zeroed: equal weight, cash included";
}

TEST_F(BPGVRotationTest, TheVolTargetScalesEveryWeightBetweenItsFloorAndOne) {
    auto cfg = make_config();
    cfg.vol_target.enabled = true;
    cfg.vol_target.use_garch_natural_vol = true;  // target 10%, floor 0.2
    auto s = make_running_strategy("VOL_TARGET", db_, cfg);
    auto scaled = [&](double sigma) {
        s->current_garch_sigma_natural_ = sigma;
        std::unordered_map<std::string, double> w{{"SPY", 0.6}, {"TLT", 0.4}};
        s->apply_portfolio_vol_target(w);
        return w;
    };
    EXPECT_NEAR(scaled(0.20).at("SPY"), 0.30, 1e-12) << "10% / 20% = 0.5";
    EXPECT_NEAR(scaled(0.20).at("TLT"), 0.20, 1e-12);
    EXPECT_NEAR(scaled(1.00).at("SPY"), 0.6 * 0.2, 1e-12) << "never below the floor";
    EXPECT_DOUBLE_EQ(scaled(0.05).at("SPY"), 0.6) << "never above 1: no leverage";
}

TEST_F(BPGVRotationTest, MaxRiskSigmaTakesTheLargestCandidateOnlyWhileStressIsConfirmed) {
    auto cfg = make_config();
    cfg.vol_target.enabled = true;
    cfg.vol_target.use_garch_natural_vol = true;
    cfg.vol_target.use_max_risk_sigma = true;
    cfg.vol_target.conditional_max_risk_sigma = true;
    cfg.vol_target.include_realized_portfolio_sigma = false;
    cfg.vol_target.include_defensive_pair_sigma = false;
    cfg.market_stress.enabled = true;
    auto s = make_running_strategy("MAX_RISK", db_, cfg);
    s->current_garch_sigma_natural_ = 0.20;

    set_history(*s, "SPY", zigzag(230, 100.0, 0.02));  // volatile but not down 8%: no stress
    std::unordered_map<std::string, double> calm{{"SPY", 1.0}};
    s->apply_portfolio_vol_target(calm);
    EXPECT_NEAR(calm.at("SPY"), 0.5, 1e-12) << "no confirmed stress: the GARCH sigma alone";

    set_history(*s, "SPY", stressed_spy());
    ASSERT_TRUE(s->is_market_stress_active());
    const double spy_sigma = s->compute_symbol_vol(s->symbol_state_.at("SPY").price_history, 21);
    ASSERT_GT(spy_sigma, 0.20);
    std::unordered_map<std::string, double> stressed{{"SPY", 1.0}};
    s->apply_portfolio_vol_target(stressed);
    EXPECT_NEAR(stressed.at("SPY"), 0.10 / spy_sigma, 1e-12) << "the stress symbol's larger sigma";
}

TEST_F(BPGVRotationTest, MarketStressHalvesRiskOnWhileSpyIsDownAndBelowItsSmaAndThenLifts) {
    auto cfg = make_config();
    cfg.market_stress.enabled = true;  // -8% over 21 days and below the 200-day SMA: x0.5
    auto s = make_running_strategy("STRESS", db_, cfg);
    set_history(*s, "TLT", {100.0});
    set_history(*s, "SPY", stressed_spy());
    s->pre_market_stress_weights_ = {{"SPY", 0.3}, {"TLT", 0.2}};
    s->refresh_market_stress_overlay();
    EXPECT_NEAR(s->get_current_weights().at("SPY"), 0.15, 1e-12);
    EXPECT_DOUBLE_EQ(s->get_current_weights().at("TLT"), 0.2) << "the risk-off sleeve is untouched";

    set_history(*s, "SPY", ramp(251, 100.0, 0.001));
    s->refresh_market_stress_overlay();
    EXPECT_DOUBLE_EQ(s->get_current_weights().at("SPY"), 0.3)
        << "re-applied from the rebalance weights: the haircut lifts and never compounds";
}

TEST_F(BPGVRotationTest, MaxRiskSigmaTakesTheLargestOfAllFourCandidates) {
    auto cfg = make_config();
    cfg.vol_target.enabled = true;
    cfg.vol_target.use_garch_natural_vol = true;
    cfg.vol_target.use_max_risk_sigma = true;  // unconditional: always armed
    auto s = make_running_strategy("FOUR_SIGMAS", db_, cfg);
    s->current_garch_sigma_natural_ = 0.05;
    const auto book = zigzag(70, 100000.0, 0.02);  // the book's own value series
    s->portfolio_value_history_ = book;
    set_history(*s, "SPY", zigzag(30, 100.0, 0.01));
    auto scaled = [&](double tlt_amplitude, double gld_amplitude) {
        set_history(*s, "TLT", zigzag(70, 100.0, tlt_amplitude));
        set_history(*s, "GLD", zigzag(70, 100.0, gld_amplitude));
        std::unordered_map<std::string, double> w{{"SPY", 0.6}, {"TLT", 0.2}, {"GLD", 0.2}};
        s->apply_portfolio_vol_target(w);
        return w.at("SPY") / 0.6;
    };

    // A volatile defensive pair: its equal-weighted return swings 3% a day, the largest sigma.
    const auto pair = zigzag(70, 100.0, 0.03);
    EXPECT_NEAR(scaled(0.03, 0.03), 0.10 / annualised_sd(pair, 60), 1e-9);

    // A calm pair: the book's realized volatility is now the largest.
    const double realized = annualised_sd(book, 60);
    ASSERT_GT(realized, annualised_sd(s->symbol_state_.at("SPY").price_history, 21));
    EXPECT_NEAR(scaled(0.002, 0.002), 0.10 / realized, 1e-9);

    // Without max-risk and without a GARCH value that month, the realized path alone applies.
    s->bpgv_config_.vol_target.use_max_risk_sigma = false;
    s->current_garch_sigma_natural_ = 0.0;
    EXPECT_NEAR(scaled(0.03, 0.03), 0.10 / realized, 1e-9) << "no GARCH: the book's own volatility";
}

TEST_F(BPGVRotationTest, TheMonthsGarchSetsTheDynamicTargetAndAMissingMonthFallsBack) {
    auto cfg = make_config();
    cfg.vol_target.enabled = true;
    cfg.vol_target.enable_dynamic_target = true;  // 12% at percentile <= 25, 6% at >= 75
    auto s = make_running_strategy("DYNAMIC", db_, cfg);
    auto month = [](int year, int m, double garch, double pct) {
        MonthlyMacroRecord r;
        r.year = year;
        r.month = m;
        r.bpgv_garch = garch;
        r.bpgv_garch_percentile = pct;
        return r;
    };
    s->macro_data_ = {month(2024, 1, 0.02, 80.0), month(2024, 2, 0.03, 50.0),
                      month(2024, 3, 0.01, 20.0)};

    s->execute_rebalance(2024, 2);  // reads January
    EXPECT_DOUBLE_EQ(s->current_vol_target_, 0.06) << "a high GARCH percentile: the tightest target";
    EXPECT_NEAR(s->current_garch_sigma_natural_, 0.02 * std::sqrt(12.0), 1e-12);
    s->execute_rebalance(2024, 3);  // reads February
    EXPECT_NEAR(s->current_vol_target_, 0.09, 1e-12) << "halfway between the 25th and 75th";
    s->execute_rebalance(2024, 4);  // reads March
    EXPECT_DOUBLE_EQ(s->current_vol_target_, 0.12) << "a low GARCH percentile: the loosest target";

    s->execute_rebalance(2024, 6);  // May is missing: falls back to the latest earlier month
    EXPECT_DOUBLE_EQ(s->current_vol_target_, 0.12);
    EXPECT_EQ(s->get_rebalance_count(), 4);

    s->execute_rebalance(2023, 12);  // nothing at or before November 2023: skipped
    EXPECT_EQ(s->get_rebalance_count(), 4);
}
