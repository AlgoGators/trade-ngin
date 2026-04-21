#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <cmath>
#include <deque>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/strategy/bpgv_rotation.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

BPGVRotationConfig make_valid_bpgv_config() {
    BPGVRotationConfig cfg;
    cfg.macro_csv_path = "data/macro/bpgv_regime.csv";
    cfg.risk_on_symbols = {"SPY", "QQQ", "XLK", "SMH", "IWM", "XHB", "IYR", "EQR"};
    // Remediation Fix 1: TLT/GLD only in the risk-off bucket; BIL/DBMF move to cash.
    cfg.risk_off_symbols = {"TLT", "GLD"};
    cfg.cash_symbols = {"BIL", "DBMF"};
    cfg.crash_override.defensive_weights = {
        {"BIL", 0.40}, {"TLT", 0.25}, {"GLD", 0.20}, {"DBMF", 0.15}};
    cfg.crash_override.zero_symbols = {"SPY", "QQQ", "XLK", "SMH",
                                       "IWM", "XHB", "IYR", "EQR"};
    cfg.crash_override.splice_fallback_symbol = "BIL";
    return cfg;
}

StrategyConfig make_valid_strategy_config() {
    StrategyConfig sc;
    sc.asset_classes = {AssetClass::EQUITIES};
    sc.frequencies = {DataFrequency::DAILY};
    sc.capital_allocation = 100000.0;
    sc.max_leverage = 1.0;
    sc.max_drawdown = 0.5;
    for (const auto& sym : {"SPY", "QQQ", "XLK", "SMH", "IWM", "XHB", "IYR",
                            "EQR", "TLT", "GLD", "BIL", "DBMF"}) {
        sc.trading_params[sym] = 1.0;
        sc.position_limits[sym] = 10000.0;
    }
    return sc;
}

}  // namespace

// ===========================================================================
// Change 1 — CrashOverrideConfig::from_json (nested trigger/exit structs)
// ===========================================================================

class CrashOverrideConfigTest : public TestBase {};

TEST_F(CrashOverrideConfigTest, ParsesFullBlock) {
    nlohmann::json j = {
        {"defensive_weights", {{"BIL", 0.40}, {"TLT", 0.25}, {"GLD", 0.20}, {"DBMF", 0.15}}},
        {"zero_symbols", {"SPY", "QQQ", "XLK"}},
        {"splice_fallback_symbol", "BIL"},
        {"trigger", {{"method", "volatility_scaled"},
                     {"k_sigma", 2.0},
                     {"vol_gate_percentile", 0.40},
                     {"lookback_days", 60}}},
        {"exit", {{"method", "signal_contingent"},
                  {"min_hold_days", 5},
                  {"confirmation_days", 5},
                  {"exit_threshold", 0.55}}}
    };

    CrashOverrideConfig cfg;
    cfg.from_json(j);

    EXPECT_EQ(cfg.defensive_weights.size(), 4u);
    EXPECT_DOUBLE_EQ(cfg.defensive_weights.at("BIL"), 0.40);
    EXPECT_EQ(cfg.zero_symbols.size(), 3u);
    EXPECT_EQ(cfg.splice_fallback_symbol, "BIL");
    EXPECT_EQ(cfg.trigger.method, "volatility_scaled");
    EXPECT_DOUBLE_EQ(cfg.trigger.k_sigma, 2.0);
    EXPECT_DOUBLE_EQ(cfg.trigger.vol_gate_percentile, 0.40);
    EXPECT_EQ(cfg.trigger.lookback_days, 60);
    EXPECT_EQ(cfg.exit.method, "signal_contingent");
    EXPECT_EQ(cfg.exit.min_hold_days, 5);
    EXPECT_EQ(cfg.exit.confirmation_days, 5);
    EXPECT_DOUBLE_EQ(cfg.exit.exit_threshold, 0.55);
}

TEST_F(CrashOverrideConfigTest, DefaultsWhenEmpty) {
    nlohmann::json j = nlohmann::json::object();
    CrashOverrideConfig cfg;
    cfg.from_json(j);

    EXPECT_TRUE(cfg.defensive_weights.empty());
    EXPECT_EQ(cfg.splice_fallback_symbol, "BIL");
    EXPECT_EQ(cfg.trigger.method, "fixed_drawdown");
    EXPECT_EQ(cfg.exit.method, "calendar_timer");
}

TEST_F(CrashOverrideConfigTest, PromptBasketSumsToOne) {
    nlohmann::json j = {
        {"defensive_weights", {{"BIL", 0.40}, {"TLT", 0.25}, {"GLD", 0.20}, {"DBMF", 0.15}}}
    };
    CrashOverrideConfig cfg;
    cfg.from_json(j);

    double sum = 0.0;
    for (const auto& [_, w] : cfg.defensive_weights) sum += w;
    EXPECT_NEAR(sum, 1.0, 1e-9);
}

// ===========================================================================
// Change 2 — RebalanceConfig::from_json
// ===========================================================================

class RebalanceConfigTest : public TestBase {};

TEST_F(RebalanceConfigTest, ParsesFullBlock) {
    nlohmann::json j = {
        {"drift_abs_trigger", 0.015},
        {"drift_rel_trigger", 0.30},
        {"halfway_rule", false},
        {"exit_fully_on_zero_target", false},
        {"enter_fully_on_zero_current", false}
    };
    RebalanceConfig cfg;
    cfg.from_json(j);

    EXPECT_DOUBLE_EQ(cfg.drift_abs_trigger, 0.015);
    EXPECT_DOUBLE_EQ(cfg.drift_rel_trigger, 0.30);
    EXPECT_FALSE(cfg.halfway_rule);
    EXPECT_FALSE(cfg.exit_fully_on_zero_target);
    EXPECT_FALSE(cfg.enter_fully_on_zero_current);
}

TEST_F(RebalanceConfigTest, DefaultsArePromptValues) {
    RebalanceConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.drift_abs_trigger, 0.010);
    EXPECT_DOUBLE_EQ(cfg.drift_rel_trigger, 0.25);
    EXPECT_TRUE(cfg.halfway_rule);
    EXPECT_TRUE(cfg.exit_fully_on_zero_target);
    EXPECT_TRUE(cfg.enter_fully_on_zero_current);
}

TEST_F(RebalanceConfigTest, PartialOverrideKeepsOtherDefaults) {
    nlohmann::json j = {{"drift_abs_trigger", 0.005}};
    RebalanceConfig cfg;
    cfg.from_json(j);
    EXPECT_DOUBLE_EQ(cfg.drift_abs_trigger, 0.005);
    EXPECT_DOUBLE_EQ(cfg.drift_rel_trigger, 0.25);
    EXPECT_TRUE(cfg.halfway_rule);
}

// ===========================================================================
// Change 3 — CrashTriggerConfig + vol-scaled trigger defaults
// ===========================================================================

class CrashTriggerConfigTest : public TestBase {};

TEST_F(CrashTriggerConfigTest, DefaultsArePromptValues) {
    CrashTriggerConfig cfg;
    EXPECT_EQ(cfg.method, "fixed_drawdown");
    EXPECT_EQ(cfg.lookback_days, 60);
    EXPECT_DOUBLE_EQ(cfg.k_sigma, 2.0);
    EXPECT_DOUBLE_EQ(cfg.vol_gate_percentile, 0.40);
    EXPECT_EQ(cfg.vol_gate_window, 504);
    EXPECT_EQ(cfg.min_history_days, 80);
}

TEST_F(CrashTriggerConfigTest, VolatilityScaledParse) {
    nlohmann::json j = {
        {"method", "volatility_scaled"},
        {"lookback_days", 90},
        {"k_sigma", 2.5},
        {"vol_gate_percentile", 0.50},
        {"vol_gate_window", 252},
        {"min_history_days", 120}
    };
    CrashTriggerConfig cfg;
    cfg.from_json(j);
    EXPECT_EQ(cfg.method, "volatility_scaled");
    EXPECT_EQ(cfg.lookback_days, 90);
    EXPECT_DOUBLE_EQ(cfg.k_sigma, 2.5);
    EXPECT_DOUBLE_EQ(cfg.vol_gate_percentile, 0.50);
    EXPECT_EQ(cfg.vol_gate_window, 252);
    EXPECT_EQ(cfg.min_history_days, 120);
}

// ===========================================================================
// Change 4 — CrashExitConfig
// ===========================================================================

class CrashExitConfigTest : public TestBase {};

TEST_F(CrashExitConfigTest, DefaultsArePromptValues) {
    CrashExitConfig cfg;
    EXPECT_EQ(cfg.method, "calendar_timer");
    EXPECT_EQ(cfg.min_hold_days, 5);
    EXPECT_EQ(cfg.confirmation_days, 5);
    EXPECT_DOUBLE_EQ(cfg.exit_threshold, 0.55);
    EXPECT_EQ(cfg.max_hold_days, -1);
    EXPECT_DOUBLE_EQ(cfg.weight_trend, 0.50);
    EXPECT_DOUBLE_EQ(cfg.weight_vol_norm, 0.25);
    EXPECT_DOUBLE_EQ(cfg.weight_recov, 0.25);
    EXPECT_EQ(cfg.trend_symbol, "SPY");
}

TEST_F(CrashExitConfigTest, SignalContingentParse) {
    nlohmann::json j = {
        {"method", "signal_contingent"},
        {"min_hold_days", 7},
        {"confirmation_days", 3},
        {"exit_threshold", 0.60},
        {"max_hold_days", nullptr},
        {"trend_symbol", "QQQ"},
        {"weights", {{"trend", 0.60}, {"vol_norm", 0.20}, {"recov", 0.20}}}
    };
    CrashExitConfig cfg;
    cfg.from_json(j);
    EXPECT_EQ(cfg.method, "signal_contingent");
    EXPECT_EQ(cfg.min_hold_days, 7);
    EXPECT_EQ(cfg.confirmation_days, 3);
    EXPECT_DOUBLE_EQ(cfg.exit_threshold, 0.60);
    EXPECT_EQ(cfg.max_hold_days, -1);  // null → -1 (no forced exit)
    EXPECT_EQ(cfg.trend_symbol, "QQQ");
    EXPECT_DOUBLE_EQ(cfg.weight_trend, 0.60);
    EXPECT_DOUBLE_EQ(cfg.weight_vol_norm, 0.20);
    EXPECT_DOUBLE_EQ(cfg.weight_recov, 0.20);
}

// ===========================================================================
// Change 5 — MomentumConfig
// ===========================================================================

class MomentumConfigTest : public TestBase {};

TEST_F(MomentumConfigTest, DefaultsArePromptValues) {
    MomentumConfig cfg;
    EXPECT_TRUE(cfg.tsm_gate_enabled);
    EXPECT_EQ(cfg.tsm_lookback_days, 252);
    EXPECT_EQ(cfg.tsm_risk_free_symbol, "BIL");
    EXPECT_TRUE(cfg.tsm_fail_open_on_short_history);
    EXPECT_EQ(cfg.xsec_lookback_days, 126);
    EXPECT_TRUE(cfg.xsec_use_vol_scaling);
    EXPECT_EQ(cfg.xsec_vol_window_days, 126);
    EXPECT_DOUBLE_EQ(cfg.xsec_sigma_floor, 0.05);
    EXPECT_DOUBLE_EQ(cfg.xsec_tau, 0.40);
    EXPECT_DOUBLE_EQ(cfg.xsec_weight_floor, 0.50);
}

TEST_F(MomentumConfigTest, FullParse) {
    nlohmann::json j = {
        {"tsm_gate_enabled", false},
        {"tsm_lookback_days", 189},
        {"tsm_risk_free_symbol", "SHV"},
        {"xsec_lookback_days", 63},
        {"xsec_use_vol_scaling", false},
        {"xsec_tau", 0.30}
    };
    MomentumConfig cfg;
    cfg.from_json(j);
    EXPECT_FALSE(cfg.tsm_gate_enabled);
    EXPECT_EQ(cfg.tsm_lookback_days, 189);
    EXPECT_EQ(cfg.tsm_risk_free_symbol, "SHV");
    EXPECT_EQ(cfg.xsec_lookback_days, 63);
    EXPECT_FALSE(cfg.xsec_use_vol_scaling);
    EXPECT_DOUBLE_EQ(cfg.xsec_tau, 0.30);
}

// ===========================================================================
// Change 6 — BreakoutConfig + Wilder ATR
// ===========================================================================

class BreakoutConfigTest : public TestBase {};

TEST_F(BreakoutConfigTest, DefaultsArePromptValues) {
    BreakoutConfig cfg;
    EXPECT_EQ(cfg.mode, "graded");
    EXPECT_EQ(cfg.sma_window, 200);
    EXPECT_EQ(cfg.atr_window, 14);
    EXPECT_DOUBLE_EQ(cfg.atr_k, 2.0);
    EXPECT_EQ(cfg.confirmation_days, 3);
    EXPECT_TRUE(cfg.graded_weighting);
    EXPECT_TRUE(cfg.index_gate_enabled);
    EXPECT_EQ(cfg.index_gate_symbol, "SPY");
    EXPECT_DOUBLE_EQ(cfg.index_gate_floor, 0.25);
}

TEST_F(BreakoutConfigTest, BinaryLegacyMode) {
    nlohmann::json j = {{"mode", "binary_legacy"}};
    BreakoutConfig cfg;
    cfg.from_json(j);
    EXPECT_EQ(cfg.mode, "binary_legacy");
    // Other values keep defaults.
    EXPECT_EQ(cfg.sma_window, 200);
}

// Test the Wilder ATR helper on a hand-computed reference series. Expose via
// a local reimplementation matching the production impl so the test doesn't
// depend on a friend declaration — this documents the expected math.
namespace {

double reference_wilder_atr(const std::deque<double>& high,
                            const std::deque<double>& low,
                            const std::deque<double>& close,
                            int period) {
    int n = static_cast<int>(close.size());
    if (n <= period) return 0.0;
    std::vector<double> tr;
    for (int i = 1; i < n; ++i) {
        double h_l = high[i] - low[i];
        double h_pc = std::abs(high[i] - close[i - 1]);
        double l_pc = std::abs(low[i] - close[i - 1]);
        tr.push_back(std::max({h_l, h_pc, l_pc}));
    }
    double atr = 0.0;
    for (int i = 0; i < period; ++i) atr += tr[i];
    atr /= period;
    for (size_t i = period; i < tr.size(); ++i) {
        atr = ((period - 1) * atr + tr[i]) / period;
    }
    return atr;
}

}  // namespace

TEST(WilderATRTest, MatchesReferenceOnConstantTR) {
    // Constant TR series: H-L = 2 every bar, no gaps → ATR should equal 2.
    std::deque<double> high, low, close;
    double base = 100.0;
    for (int i = 0; i < 30; ++i) {
        double c = base + i;
        high.push_back(c + 1.0);
        low.push_back(c - 1.0);
        close.push_back(c);
    }
    double atr = reference_wilder_atr(high, low, close, 14);
    EXPECT_NEAR(atr, 2.0, 1e-6);
}

TEST(WilderATRTest, HandlesGappingBars) {
    // Gap-up scenario: close_{i-1} = 100, next bar [H=110, L=105, C=108].
    // TR should be max(110-105, |110-100|, |105-100|) = 10.
    std::deque<double> high = {100, 110};
    std::deque<double> low = {100, 105};
    std::deque<double> close = {100, 108};
    // With only 1 TR sample, need period = 1 to get a meaningful seed.
    double atr = reference_wilder_atr(high, low, close, 1);
    EXPECT_NEAR(atr, 10.0, 1e-6);
}

// ===========================================================================
// Strategy construction + validate_config (Change 1 guardrail)
// ===========================================================================

class BPGVTier1ValidationTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
        ASSERT_TRUE(db_->connect().is_ok());
    }
    void TearDown() override {
        if (db_) db_->disconnect();
        TestBase::TearDown();
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(BPGVTier1ValidationTest, ValidConfigInitializesSuccessfully) {
    auto bpgv_cfg = make_valid_bpgv_config();
    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_VALID", strat_cfg, bpgv_cfg, db_);
    auto result = strategy->initialize();
    EXPECT_TRUE(result.is_ok())
        << "Expected successful init; got: "
        << (result.is_error() ? result.error()->what() : "ok");
}

TEST_F(BPGVTier1ValidationTest, MissingDefensiveWeightsFailsLoudly) {
    auto bpgv_cfg = make_valid_bpgv_config();
    bpgv_cfg.crash_override.defensive_weights.clear();
    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_NO_DEFENSIVE", strat_cfg, bpgv_cfg, db_);
    auto result = strategy->initialize();
    ASSERT_TRUE(result.is_error());
    std::string err = result.error()->what();
    EXPECT_NE(err.find("defensive_weights"), std::string::npos)
        << "Error should mention defensive_weights; got: " << err;
}

TEST_F(BPGVTier1ValidationTest, DefensiveWeightsNotSummingToOneFails) {
    auto bpgv_cfg = make_valid_bpgv_config();
    bpgv_cfg.crash_override.defensive_weights = {
        {"BIL", 0.40}, {"TLT", 0.25}, {"GLD", 0.15}};
    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_BAD_SUM", strat_cfg, bpgv_cfg, db_);
    auto result = strategy->initialize();
    ASSERT_TRUE(result.is_error());
    std::string err = result.error()->what();
    EXPECT_NE(err.find("sum to 1.0"), std::string::npos);
}

TEST_F(BPGVTier1ValidationTest, PromptBasketPassesValidation) {
    auto bpgv_cfg = make_valid_bpgv_config();
    EXPECT_DOUBLE_EQ(bpgv_cfg.crash_override.defensive_weights.at("BIL"), 0.40);
    EXPECT_DOUBLE_EQ(bpgv_cfg.crash_override.defensive_weights.at("TLT"), 0.25);
    EXPECT_DOUBLE_EQ(bpgv_cfg.crash_override.defensive_weights.at("GLD"), 0.20);
    EXPECT_DOUBLE_EQ(bpgv_cfg.crash_override.defensive_weights.at("DBMF"), 0.15);

    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_PROMPT_BASKET", strat_cfg, bpgv_cfg, db_);
    auto result = strategy->initialize();
    EXPECT_TRUE(result.is_ok());
}

// ===========================================================================
// Integration — strategy with all Tier-1 nested configs initializes cleanly
// ===========================================================================

TEST_F(BPGVTier1ValidationTest, FullTier1ConfigInitializes) {
    auto bpgv_cfg = make_valid_bpgv_config();
    // Flip on all Tier-1 mechanisms.
    bpgv_cfg.crash_override.trigger.method = "volatility_scaled";
    bpgv_cfg.crash_override.exit.method = "signal_contingent";
    bpgv_cfg.momentum.tsm_gate_enabled = true;
    bpgv_cfg.breakout.mode = "graded";

    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_TIER1_FULL", strat_cfg, bpgv_cfg, db_);
    auto result = strategy->initialize();
    EXPECT_TRUE(result.is_ok())
        << "Full Tier-1 config should initialize; got: "
        << (result.is_error() ? result.error()->what() : "ok");

    // Lookback must accommodate the 504-day vol-gate window.
    EXPECT_GE(strategy->get_max_required_lookback(), 504);
}

// ===========================================================================
// Remediation Fix 1 — cash_symbols bucket
// ===========================================================================

TEST_F(BPGVTier1ValidationTest, CashBucketInitializesSymbolsAndPositions) {
    auto bpgv_cfg = make_valid_bpgv_config();
    // Deliberately leave cash_symbols = {BIL, DBMF} from the factory.
    auto strat_cfg = make_valid_strategy_config();

    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_CASH_BUCKET", strat_cfg, bpgv_cfg, db_);
    ASSERT_TRUE(strategy->initialize().is_ok());

    // get_price_history should expose BOTH risk-on, risk-off, AND cash symbols.
    auto history = strategy->get_price_history();
    EXPECT_NE(history.find("SPY"), history.end());
    EXPECT_NE(history.find("TLT"), history.end());
    EXPECT_NE(history.find("BIL"), history.end());
    EXPECT_NE(history.find("DBMF"), history.end());
}

TEST_F(BPGVTier1ValidationTest, CashSymbolsAppearInTargetPositions) {
    // The target positions map should emit entries for cash symbols so the
    // executor can close them out when the crash override ends.
    auto bpgv_cfg = make_valid_bpgv_config();
    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_CASH_TARGETS", strat_cfg, bpgv_cfg, db_);
    ASSERT_TRUE(strategy->initialize().is_ok());

    auto targets = strategy->get_target_positions();
    EXPECT_NE(targets.find("BIL"), targets.end());
    EXPECT_NE(targets.find("DBMF"), targets.end());
    // No live bars fed yet + no override → qty should be 0.
    EXPECT_DOUBLE_EQ(targets.at("BIL").quantity.as_double(), 0.0);
    EXPECT_DOUBLE_EQ(targets.at("DBMF").quantity.as_double(), 0.0);
}

// ===========================================================================
// Remediation Fix 2 — MomentumConfig::tsm_tolerance
// ===========================================================================

TEST_F(MomentumConfigTest, DefaultTSMToleranceIsZero) {
    // Back-compat: default must preserve Tier-1 strict gate behaviour.
    MomentumConfig cfg;
    EXPECT_DOUBLE_EQ(cfg.tsm_tolerance, 0.0);
}

TEST_F(MomentumConfigTest, ToleranceParse) {
    nlohmann::json j = {{"tsm_tolerance", 0.05}};
    MomentumConfig cfg;
    cfg.from_json(j);
    EXPECT_DOUBLE_EQ(cfg.tsm_tolerance, 0.05);
}

// ===========================================================================
// Remediation Fix 3 — warmup pre-load at initialize()
// ===========================================================================

TEST_F(BPGVTier1ValidationTest, WarmupPreloadFillsHistories) {
    // Stand up a strategy whose warmup_start_date is recent enough that the
    // CSV-seeded tickers (IWM/IYR/QQQ/SMH/XHB/XLK/BIL/DBMF) have data
    // available in the window [warmup_start - warmup_days, warmup_start - 1d].
    //
    // Gotcha: MockPostgresDatabase::get_market_data returns a fixed 2-row
    // stub regardless of the date window (see tests/data/test_db_utils.hpp:105).
    // To exercise the real CSV fallback path, disconnect the mock before
    // initialize() so the `db_->is_connected()` guard short-circuits and the
    // strategy falls through to CSVEquityLoader.
    auto bpgv_cfg = make_valid_bpgv_config();
    std::tm tm{};
    tm.tm_year = 2022 - 1900;
    tm.tm_mon = 0;
    tm.tm_mday = 3;  // Jan 3 2022 (trading day)
    std::time_t tt = std::mktime(&tm);
    bpgv_cfg.warmup_start_date = std::chrono::system_clock::from_time_t(tt);
    bpgv_cfg.warmup_days = 200;

    db_->disconnect();  // force CSV path

    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_WARMUP", strat_cfg, bpgv_cfg, db_);
    ASSERT_TRUE(strategy->initialize().is_ok());

    auto history = strategy->get_price_history();
    // For CSV-seeded QQQ we expect a meaningful amount of pre-loaded data.
    // Allowing slack for weekends / holidays — 200 calendar days is roughly
    // 140 trading days. Require at least 100.
    ASSERT_NE(history.find("QQQ"), history.end());
    EXPECT_GE(history.at("QQQ").size(), static_cast<size_t>(100))
        << "Warmup pre-load should have filled QQQ with ~140 bars; got "
        << history.at("QQQ").size();
    // BIL is also CSV-seeded.
    ASSERT_NE(history.find("BIL"), history.end());
    EXPECT_GE(history.at("BIL").size(), static_cast<size_t>(100));
}

TEST_F(BPGVTier1ValidationTest, WarmupDisabledWhenStartDateZero) {
    // Explicitly disable the pre-load by leaving warmup_start_date at its
    // zero-Timestamp default. Histories should all be empty after init.
    auto bpgv_cfg = make_valid_bpgv_config();
    // bpgv_cfg.warmup_start_date stays default (zero time point).
    auto strat_cfg = make_valid_strategy_config();
    auto strategy = std::make_unique<BPGVRotationStrategy>(
        "TEST_BPGV_WARMUP_OFF", strat_cfg, bpgv_cfg, db_);
    ASSERT_TRUE(strategy->initialize().is_ok());

    auto history = strategy->get_price_history();
    // All symbols should have empty price histories since no bars fed and
    // the pre-load path is gated off.
    EXPECT_TRUE(history.at("QQQ").empty());
    EXPECT_TRUE(history.at("BIL").empty());
}

// ===========================================================================
// Remediation Fix 4 — wider tolerance bands
// ===========================================================================

TEST_F(RebalanceConfigTest, WideBandsActive) {
    nlohmann::json j = {
        {"drift_abs_trigger", 0.025},
        {"drift_rel_trigger", 0.50},
        {"halfway_rule", true},
        {"exit_fully_on_zero_target", true},
        {"enter_fully_on_zero_current", true}
    };
    RebalanceConfig cfg;
    cfg.from_json(j);
    EXPECT_DOUBLE_EQ(cfg.drift_abs_trigger, 0.025);
    EXPECT_DOUBLE_EQ(cfg.drift_rel_trigger, 0.50);
    EXPECT_TRUE(cfg.halfway_rule);
}
