// include/trade_ngin/strategy/bpgv_rotation.hpp
#pragma once

#include <algorithm>
#include <deque>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/macro_csv_loader.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

/**
 * @brief Crash-override trigger config (Change 3).
 *
 * `method = "fixed_drawdown"` reproduces the baseline fixed-threshold rule;
 * `method = "volatility_scaled"` uses a z-score against a 60-day realized-vol
 * estimate, gated by a vol-percentile filter so low-vol regimes don't fire
 * false positives (Moreira-Muir 2017, Harvey et al. 2018, Hocquard-Ng-Papageorgiou 2013).
 */
struct CrashTriggerConfig {
    std::string method{"fixed_drawdown"};  // "fixed_drawdown" | "volatility_scaled"
    // volatility_scaled parameters:
    int lookback_days{60};          // realized-vol estimation window
    double k_sigma{2.0};            // fire when z_5d < -k_sigma
    double vol_gate_percentile{0.40};
    int vol_gate_window{504};       // ~2 years for percentile rank
    int min_history_days{80};       // minimum history before trigger is armed
    void from_json(const nlohmann::json& j);
};

/**
 * @brief Crash-override exit config (Change 4).
 *
 * `method = "calendar_timer"` reproduces the 14-day fixed-hold exit;
 * `method = "signal_contingent"` exits when a trend+vol+recovery composite
 * holds above threshold for N consecutive days (Hoffstein-Sibears-Faber 2019
 * on rebalance-timing-luck; Antonacci 2014, Faber 2007 on signal-based exits).
 */
struct CrashExitConfig {
    std::string method{"calendar_timer"};  // "calendar_timer" | "signal_contingent"
    // signal_contingent parameters:
    int min_hold_days{5};
    int confirmation_days{5};
    double exit_threshold{0.55};
    int max_hold_days{-1};  // -1 = no forced exit
    double weight_trend{0.50};
    double weight_vol_norm{0.25};
    double weight_recov{0.25};
    std::string trend_symbol{"SPY"};  // symbol for trend/vol components
    void from_json(const nlohmann::json& j);
};

/**
 * @brief Tier-1 crash-override configuration.
 *
 * Holds the defensive basket used when the override fires, plus the trigger
 * and exit sub-configs.
 */
struct CrashOverrideConfig {
    // Defensive basket during override — symbol -> target weight. MUST sum to 1.0
    // across symbols actually in the universe. Missing-price splice shifts the
    // missing symbol's weight to `splice_fallback_symbol`.
    std::unordered_map<std::string, double> defensive_weights;

    // Symbols forced to weight 0 during override (e.g. all risk-on equities).
    std::vector<std::string> zero_symbols;

    // When a defensive_weights symbol has no live price at override entry, add
    // its weight to this symbol instead. Default "BIL".
    std::string splice_fallback_symbol{"BIL"};

    // Trigger and exit sub-configs (Group B).
    CrashTriggerConfig trigger;
    CrashExitConfig exit;

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Tier-1 tolerance-band rebalancing configuration (Change 2).
 *
 * Masters (2003) halfway rule applied inside get_target_positions() to damp the
 * daily share-count drift caused by live price moves against a fixed-weight
 * target.
 */
struct RebalanceConfig {
    // Trade only when |w_current - w_target| > drift_abs_trigger OR
    // |drift / w_target| > drift_rel_trigger. 100 bps / 25 % defaults per Masters.
    double drift_abs_trigger{0.010};
    double drift_rel_trigger{0.25};

    // If true, trade halfway back to target; if false, trade all the way to target.
    bool halfway_rule{true};

    // Always trade fully to zero when target = 0 and current > 0 (decisive exit).
    bool exit_fully_on_zero_target{true};

    // Always trade fully to target when current = 0 and target > 0 (no partial entry).
    bool enter_fully_on_zero_current{true};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Momentum pipeline config (Change 5).
 *
 * Replaces the 63-day raw-return xsec ranker with:
 *   1. 12-month absolute time-series momentum gate (Moskowitz-Ooi-Pedersen 2012,
 *      Antonacci 2014) — zero out any risk-on name with negative excess return.
 *   2. 126-day vol-scaled cross-sectional score r/sigma (Barroso-Santa-Clara 2015,
 *      Daniel-Moskowitz 2016 on momentum crashes).
 */
struct MomentumConfig {
    bool tsm_gate_enabled{true};
    int tsm_lookback_days{252};
    std::string tsm_risk_free_symbol{"BIL"};
    bool tsm_fail_open_on_short_history{true};

    // Tier-1 remediation (Fix 2): allow names through the TSM gate if their
    // 12 m excess return is within this tolerance of zero. Gate fires when
    // excess < -tsm_tolerance. Default 0.0 preserves Tier-1 strict gate;
    // active config sets 0.05 (accept names up to 5 % below risk-free).
    double tsm_tolerance{0.0};

    int xsec_lookback_days{126};
    bool xsec_use_vol_scaling{true};
    int xsec_vol_window_days{126};
    double xsec_sigma_floor{0.05};
    double xsec_tau{0.40};
    double xsec_weight_floor{0.50};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Breakout filter config (Change 6).
 *
 * Replaces the binary 50-day SMA cut with:
 *   1. 200-day SMA (Faber 2007/2013 — 5-10x fewer whipsaws).
 *   2. 3-bar confirmation before zero-out.
 *   3. Graded (close - SMA) / (k * ATR_14) score clipped to [0, 1] (Wilder ATR,
 *      Hurst-Ooi-Pedersen 2017 vol-sized continuous exposure).
 *   4. Index gate — multiply all risk-on by SPY's own graded score, floored at
 *      `index_gate_floor` (Asness-Moskowitz-Pedersen 2013 on broad-market gates).
 */
struct BreakoutConfig {
    std::string mode{"graded"};  // "graded" | "binary_legacy"
    int sma_window{200};
    int atr_window{14};
    double atr_k{2.0};
    int confirmation_days{3};
    bool graded_weighting{true};

    bool index_gate_enabled{true};
    std::string index_gate_symbol{"SPY"};
    double index_gate_floor{0.25};

    bool fail_open_on_short_history{true};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Configuration for the BPGV macro regime rotation strategy.
 */
struct BPGVRotationConfig {
    // Macro data source
    std::string macro_csv_path{"data/macro/bpgv_regime.csv"};

    // Rebalancing
    int rebalance_day_of_month{18};  // Rebalance on day >= this (post-permit-release)

    // Allocation interpolation (risk_off_weight = min + range * ((score+1)/2))
    double base_risk_off_min{0.05};    // 5% defensive floor
    double base_risk_off_range{0.40};  // 5%-45% range

    // Strong risk-on boost (no leverage — just tilt within portfolio)
    double strong_risk_on_equity_boost{0.12};
    double strong_risk_on_bond_reduction{0.20};

    // Momentum tilt
    int momentum_lookback_days{63};      // ~3 months trading days
    double momentum_tilt_scale{0.40};    // +/-40% tilt by rank

    // Homebuilder/housing tilt
    double homebuilder_tilt_scale{0.20};   // +/-20% based on permit growth
    std::string homebuilder_symbol{"XHB"}; // Symbol to tilt (XHB or HD as proxy)

    // Breakout filter (single configurable SMA window)
    int breakout_sma_window{50};

    // Crash override
    double crash_threshold{-0.07};         // -7% drawdown in lookback window
    int crash_lookback_days{5};            // 5-day window for crash detection
    int crash_override_calendar_days{14};  // Hold defensive for 14 calendar days
    double crash_defensive_weight{0.45};   // 45% bonds/gold during crash

    // Asset universe
    std::vector<std::string> risk_on_symbols{"SPY", "QQQ", "XLK", "SMH", "IWM", "XHB", "IYR", "EQR"};
    std::vector<std::string> risk_off_symbols{"TLT", "GLD"};

    // Tier-1 remediation: cash symbols participate ONLY in the crash-override
    // defensive basket and as the TSM risk-free reference. They receive zero
    // base weight during normal regimes, unlike risk_off_symbols which get
    // 1.25 %–17.5 % per symbol depending on the regime score. This keeps
    // TLT+GLD's share of the risk-off bucket undiluted.
    std::vector<std::string> cash_symbols{"BIL", "DBMF"};

    // Tier-1 remediation: pre-load historical price data at initialize() so
    // long-lookback filters (252d TSM, 200d SMA, 504d vol gate) fire on day 1
    // instead of waiting for the backtest to accumulate history. Set from
    // bt_bpgv_rotation.cpp = start_date; initialize() pre-loads
    // [warmup_start_date - warmup_days, warmup_start_date - 1 day]. Passing a
    // zero/default Timestamp disables the pre-load (back-compat).
    Timestamp warmup_start_date{};
    int warmup_days{520};

    // Per-asset extreme allocation weights (risk-on extreme and risk-off extreme)
    // If empty, equal weight within each bucket is used.
    std::unordered_map<std::string, double> risk_on_extreme_weights;
    std::unordered_map<std::string, double> risk_off_extreme_weights;

    // Crash override weights (symbol -> weight). If empty, uses default split.
    // Retained for backward compat with the legacy `crash_weights` JSON key; the
    // Tier-1 path loads `crash_override.defensive_weights` into `crash_override`
    // below instead.
    std::unordered_map<std::string, double> crash_weights;

    // Tier-1 nested configs.
    CrashOverrideConfig crash_override;  // Changes 1, 3, 4
    RebalanceConfig rebalance;           // Change 2
    MomentumConfig momentum;             // Change 5
    BreakoutConfig breakout;             // Change 6

    // Position sizing
    bool allow_fractional_shares{true};
};

/**
 * @brief BPGV Macro Regime Rotation Strategy
 *
 * A portfolio-level allocation strategy that rotates between risk-on and risk-off
 * ETFs based on a macro regime score derived from:
 *   1. Building Permit Growth Volatility (BPGV) percentile
 *   2. Yield curve (10Y-2Y) spread
 *   3. BPGV EWMA trend direction
 *
 * Features:
 *   - Smooth interpolation between risk-on and risk-off extreme allocations
 *   - Momentum tilt: overweights higher-momentum risk-on assets
 *   - Homebuilder tilt: overweights XHB when permits are growing
 *   - Breakout filter: zeros weight for risk-on assets below SMA
 *   - Crash override: shifts to defensive allocation on large drawdowns
 *   - Monthly mid-month rebalancing (day >= 18, uses previous month's regime)
 */
class BPGVRotationStrategy : public BaseStrategy {
public:
    BPGVRotationStrategy(std::string id, StrategyConfig config, BPGVRotationConfig bpgv_config,
                         std::shared_ptr<PostgresDatabase> db,
                         std::shared_ptr<InstrumentRegistry> registry = nullptr);

    Result<void> initialize() override;
    Result<void> on_data(const std::vector<Bar>& data) override;

    // Targets are computed on demand from current_weights_ + live prices, so the
    // strategy's positions_ map is owned exclusively by on_execution fills.
    // Writing targets directly into positions_ (as the original implementation did)
    // double-counted against the subsequent exec fed back by the portfolio layer.
    std::unordered_map<std::string, Position> get_target_positions() const override;

    std::unordered_map<std::string, std::vector<double>> get_price_history() const override;

    int get_crash_override_count() const { return crash_override_count_; }

    int get_max_required_lookback() const {
        // Tier-1 introduced longer lookbacks: 252-day TSM gate, 200-day SMA,
        // 504-day vol percentile. Take the max so the warm-up phase has
        // enough history to arm every filter.
        int momentum = std::max({bpgv_config_.momentum_lookback_days,
                                 bpgv_config_.momentum.tsm_lookback_days,
                                 bpgv_config_.momentum.xsec_lookback_days,
                                 bpgv_config_.momentum.xsec_vol_window_days});
        int breakout = std::max({bpgv_config_.breakout_sma_window,
                                 bpgv_config_.breakout.sma_window,
                                 bpgv_config_.breakout.atr_window});
        int crash = bpgv_config_.crash_lookback_days + 10;
        if (bpgv_config_.crash_override.trigger.method == "volatility_scaled") {
            crash = std::max(crash, bpgv_config_.crash_override.trigger.vol_gate_window + 10);
        }
        return std::max({momentum, breakout, crash});
    }

protected:
    Result<void> validate_config() const override;

private:
    BPGVRotationConfig bpgv_config_;
    std::shared_ptr<InstrumentRegistry> registry_;

    // Pre-loaded macro regime data from CSV
    std::vector<MonthlyMacroRecord> macro_data_;

    // Per-symbol daily state. OHLC histories added in Change 6 for the ATR
    // breakout filter; close = price_history retained for SMA / momentum paths.
    struct SymbolState {
        std::deque<double> price_history;  // close
        std::deque<double> high_history;
        std::deque<double> low_history;
        double current_price{0.0};
        Timestamp last_update;
    };
    std::unordered_map<std::string, SymbolState> symbol_state_;

    // Portfolio-level allocation state
    std::unordered_map<std::string, double> current_weights_;
    double portfolio_value_{0.0};

    // Rebalancing tracking
    int last_rebalance_year_{0};
    int last_rebalance_month_{0};

    // Crash override state
    bool crash_override_active_{false};
    Timestamp crash_override_start_;
    std::deque<double> portfolio_value_history_;
    int crash_override_count_{0};

    // Change 4: rolling count of consecutive days the exit score has been above
    // `exit_threshold`. Reset to 0 on any failing day and on override activation.
    int consecutive_good_exit_days_{0};

    // Risk-on and risk-off extreme allocation tables (from Python V3)
    static const std::unordered_map<std::string, double> RISK_ON_EXTREME;
    static const std::unordered_map<std::string, double> RISK_OFF_EXTREME;

    // --- Core logic ---
    void update_price_histories(const std::vector<Bar>& bars);
    bool should_rebalance(int year, int month, int day) const;
    void execute_rebalance(int year, int month);

    // --- Weight calculation pipeline ---
    std::unordered_map<std::string, double> calculate_base_weights(
        const MonthlyMacroRecord& rec) const;
    void apply_momentum_tilt(std::unordered_map<std::string, double>& weights) const;
    void apply_homebuilder_tilt(std::unordered_map<std::string, double>& weights,
                                const MonthlyMacroRecord& rec) const;
    void apply_breakout_filter(std::unordered_map<std::string, double>& weights) const;
    void normalize_weights(std::unordered_map<std::string, double>& weights) const;

    // --- Crash detection ---
    bool detect_crash() const;
    void activate_crash_override(const Timestamp& ts);
    bool is_crash_override_expired(const Timestamp& ts) const;
    std::unordered_map<std::string, double> build_crash_weights() const;

    // --- Helpers ---
    double calculate_sma(const std::deque<double>& prices, int period) const;
    double calculate_trailing_return(const std::deque<double>& prices, int days) const;
    double compute_portfolio_value() const;
    void update_positions_from_weights();
    void trim_price_history(SymbolState& state) const;

    // Change 3 helpers: vol-scaled crash trigger.
    double compute_daily_return_stdev(int window) const;
    double compute_daily_return_mean(int window) const;
    double compute_sigma_percentile(int sigma_window, int percentile_window) const;

    // Change 4 helpers: signal-contingent exit score components.
    double compute_exit_score(const std::deque<double>& trend_px) const;

    // Change 5 helpers: vol-scaled xsec score.
    double compute_symbol_vol(const std::deque<double>& prices, int window) const;

    // Change 6 helper: Wilder (1978) 14-period ATR from OHL/close histories.
    static double wilder_atr(const std::deque<double>& high,
                             const std::deque<double>& low,
                             const std::deque<double>& close,
                             int period);

    // Change 6 helper: graded breakout score for one symbol.
    double breakout_score(const std::deque<double>& high,
                          const std::deque<double>& low,
                          const std::deque<double>& close) const;

    struct DateParts {
        int year;
        int month;
        int day;
    };
    static DateParts extract_date(const Timestamp& ts);

    // Bumped from 300 → 520 to accommodate the 504-day vol-gate percentile
    // window used by the volatility-scaled crash trigger (Change 3).
    static constexpr size_t MAX_PRICE_HISTORY = 520;
};

}  // namespace trade_ngin
