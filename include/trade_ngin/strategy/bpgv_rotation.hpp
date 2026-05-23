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
 * @brief Portfolio-level realized-vol targeting.
 *
 * Applied after `normalize_weights()` as a daily gross-leverage scalar
 * `s = clip(target / sigma_annualized, scalar_floor, 1.0)` on all symbol
 * weights. When the strategy's realized portfolio vol is elevated, the book
 * de-grosses toward cash; sum(weights) intentionally falls below 1.0 and the
 * residual stays uninvested.
 *
 * References:
 *   Moreira-Muir 2017 "Volatility-Managed Portfolios" (JoF),
 *   Harvey-Hoyle-Korgaonkar-Rattray-Sargaison-Hemert 2018 "Impact of
 *   Volatility Targeting".
 *
 * The Phase-4 frictionless replay (`tools/analysis/a1_candidate_screen.py::
 * c3_vol_target`) at 10% annual target produced Sharpe 0.918 (vs A1 0.815),
 * fit 0.877 / hold 0.959, 2022 cal return −19.9% vs A1 −26.8%, max DD
 * −24.3% vs A1 −32.5%. This struct holds the parameters for the C++ port of
 * that overlay.
 */
struct VolTargetConfig {
    bool enabled{false};
    double target_annualized{0.10};   // Static 10% portfolio vol target (used when enable_dynamic_target=false)
    int window_days{60};               // realized vol estimation window
    // Never scale below this fraction; avoids pathological full de-grossing
    // when the realized-vol estimate is unstable.
    double scalar_floor{0.20};
    // Need this much portfolio_value_history before scaling kicks in;
    // below the threshold, scalar = 1.0 (fail-open).
    int min_history_days{60};

    // T1.2: dynamic target driven by σ^GARCH percentile (Cortes & LaPoint
    // 2025 — high σ^BPG predicts high future return vol, so the C3 target
    // should respond to it). When enable_dynamic_target=true and the macro
    // record's bpgv_garch is valid, effective_target interpolates:
    //   percentile >= pctile_high  → target_annualized_min
    //   percentile <= pctile_low   → target_annualized_max
    //   in-between                  → linear interpolation
    // When the GARCH signal is missing (legacy CSV or pre-emission warmup),
    // falls back to the static target_annualized above.
    bool enable_dynamic_target{false};
    double target_annualized_min{0.06};   // tightest target (when σ^GARCH is HIGH)
    double target_annualized_max{0.12};   // loosest target (when σ^GARCH is LOW)
    double pctile_high{75.0};
    double pctile_low{25.0};

    // R3a (May 2026): use σ^GARCH as a paper-supported PROXY for the un-scaled
    // portfolio vol, instead of measuring it from the strategy's own return
    // series. The R1 fix (which measured σ on un-scaled returns) introduced a
    // secondary feedback loop that pinned the book at the 20% scalar floor —
    // see reports/a1_r1_r2_review.md §2 for the diagnosis. R3a decouples σ
    // estimation from the strategy's actions entirely by using the GARCH
    // forecast from the macro CSV.
    //
    // Formula:  σ_natural_annual = bpgv_garch × √12 × garch_natural_vol_multiplier
    //
    // Calibration (multiplier=1.0): backtest median bpgv_garch ≈ 3.38% monthly
    //   → σ_natural_proxy ≈ 11.69% annualized
    // vs A1's empirical un-scaled σ ≈ 13.08% — within 11% of correct.
    //
    // Expected mean scalar at this calibration with dynamic target [6%, 12%]:
    //   ≈ 0.77 (mean gross exposure ≈ 77%, floor binding only 6.7% of days).
    // Compare to R1+R2's broken state: mean scalar 0.21, floor binding 43%.
    //
    // When use_garch_natural_vol=false (default), apply_portfolio_vol_target
    // uses the original scaled-σ path (compute_daily_return_stdev) — the C9
    // working approach.
    bool use_garch_natural_vol{false};
    double garch_natural_vol_multiplier{1.0};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Vol-aware defensive sleeve composition.
 *
 * Addresses the 2022 hedge failure documented in
 * `reports/a1_drawdown_autopsy.md` (TLT lost 19.6% and GLD lost 8.0% during
 * Aug-Oct 2022 while held at ~45% each). The mechanism:
 *
 *   1. Within the risk-off bucket {TLT, GLD}, replace equal-ish weights with
 *      inverse-vol weights: w_TLT = (1/σ_TLT) / (1/σ_TLT + 1/σ_GLD).
 *   2. Synthesize the pair's daily return series and compute its 60d
 *      realized vol σ_joint.
 *   3. If σ_joint > pair_vol_target, scale the risk-off bucket total by
 *      min(1, target/σ_joint) and route the slack to `splice_symbol` (BIL).
 *
 * Step 1 redistributes within the pair when one leg's vol spikes (e.g. TLT
 * during a Fed-hike shock). Step 3 cuts total defensive duration exposure
 * when BOTH legs are elevated (the 2022 failure mode).
 */
struct DefensiveSleeveConfig {
    bool vol_aware_enabled{false};
    double pair_vol_target_annualized{0.10};
    int vol_window_days{60};
    bool use_inverse_vol_pair{true};
    std::string splice_symbol{"BIL"};
    // Cap the splice fraction so we never route more than this share of the
    // risk-off bucket to the splice symbol — guards against pathologically
    // tiny defensive sleeves when σ_joint is huge.
    double max_splice_fraction{0.80};
    int min_history_days{60};

    // R2 (deep-analysis follow-on): σ^GARCH-aware pair_vol_target. The hold-
    // half IC test (reports/a1_garch_signal_ic.csv) showed σ^GARCH passes the
    // gate STRONGLY on TLT (12m IC −0.69) and GLD (12m IC −0.80, CI excludes
    // zero) — these were the strongest predictive cells in the entire deep-
    // analysis cycle. When garch_aware_enabled=true and the macro record's
    // bpgv_garch_percentile is available, interpolate the pair_vol_target
    // between high_sigma (tight, used when σ^GARCH percentile ≥ pctile_high)
    // and low_sigma (loose, used when ≤ pctile_low). When garch is missing
    // or this flag is off, falls back to the static pair_vol_target_annualized.
    bool garch_aware_enabled{false};
    double pair_vol_target_high_sigma{0.06};  // target when σ^GARCH is HIGH (defensive bucket tighter → more BIL splice)
    double pair_vol_target_low_sigma{0.12};   // target when σ^GARCH is LOW (let the defensive pair run)
    double garch_pctile_high{75.0};
    double garch_pctile_low{25.0};
    void from_json(const nlohmann::json& j);
};

/**
 * @brief Daily broad-market stress throttle for the risk-on sleeve.
 *
 * The BPGV paper signal is a volatility/regime input, not a fast directional
 * crash detector. The May-2026 drawdown autopsy showed the worst loss came
 * from keeping a high risk-on sleeve through a broad-market break. This overlay
 * is intentionally simple and external to BPGV: when SPY is both down sharply
 * over the last month and below its 200-day average, haircut only the risk-on
 * sleeve and leave the residual as cash. The last monthly rebalance weights are
 * retained unthrottled so this daily overlay is reversible and idempotent.
 */
struct MarketStressConfig {
    bool enabled{false};
    std::string stress_symbol{"SPY"};
    int drawdown_lookback_days{21};
    double drawdown_trigger{-0.08};
    int sma_window{200};
    double risk_on_scale{0.50};
    int min_history_days{220};

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

    // A1-deep-analysis follow-on (May 2026):
    //   - VolTargetConfig (C3): portfolio-level daily vol-target scalar applied
    //     after normalize_weights; sum(weights) ≤ 1.0 by design, residual is cash.
    //   - DefensiveSleeveConfig: inverse-vol re-allocation within {TLT, GLD}
    //     plus σ_joint-driven splice to BIL when the defensive pair's realized
    //     vol exceeds target (addresses the 2022 hedge collapse).
    VolTargetConfig vol_target;
    DefensiveSleeveConfig defensive_sleeve;
    MarketStressConfig market_stress;

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

    /// Set the live portfolio equity (cash + open positions) used as the
    /// position-sizing base. The backtest coordinator pushes the running NAV in
    /// each bar BEFORE on_data / get_target_positions, so the book sizes against
    /// compounding equity instead of a frozen initial allocation. When this is
    /// never called (sizing_equity_ stays 0) sizing falls back to
    /// config_.capital_allocation, so unit tests and any non-coordinator caller
    /// keep their previous behavior.
    void set_portfolio_equity(double equity) {
        if (equity > 0.0) sizing_equity_ = equity;
    }

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
    std::unordered_map<std::string, double> pre_market_stress_weights_;
    double portfolio_value_{0.0};

    // Live portfolio NAV (cash + positions) used as the position-sizing base,
    // pushed in by the backtest coordinator via set_portfolio_equity(). 0 means
    // "unset" — get_target_positions() then falls back to config_.capital_allocation.
    double sizing_equity_{0.0};

    // T1.2: dynamic vol target derived from the macro record's σ^GARCH
    // percentile each rebalance. Initialised to vol_target.target_annualized
    // so legacy (static) mode behaves byte-identically when enable_dynamic_target
    // is false. Updated by execute_rebalance() before apply_portfolio_vol_target().
    double current_vol_target_{0.10};

    // R3a: σ^GARCH-derived natural-vol proxy, annualized. Refreshed each
    // rebalance from the macro record:
    //   current_garch_sigma_natural_ = bpgv_garch × √12 × multiplier
    // When vol_target.use_garch_natural_vol is true, apply_portfolio_vol_target
    // consumes this as the σ_annual input to the scalar formula — entirely
    // bypassing the strategy's own return-series stdev. 0.0 means "unset"
    // (macro record missing bpgv_garch or warmup), in which case the function
    // falls back to the empirical compute_daily_return_stdev path.
    mutable double current_garch_sigma_natural_{0.0};

    // R1 (May 2026 deep-analysis follow-on): track the daily scalar applied
    // by apply_portfolio_vol_target so σ can be measured on the *un-scaled*
    // return series. Without this, σ is measured on the already-scaled
    // portfolio_value_history_, creating a feedback loop that traps the book
    // at ~1/3 of intended gross exposure (see reports/a1_dynamic_voltarget_review.md
    // §5). scalar_history_ is parallel to portfolio_value_history_ — index i
    // holds the scalar that was applied on day portfolio_value_history_[i].
    // Updated each bar in on_data (push current_scalar_), and current_scalar_
    // is refreshed by apply_portfolio_vol_target on rebalance days. mutable
    // because apply_portfolio_vol_target is a const member (it consumes the
    // weights map by reference but doesn't mutate the strategy's primary
    // state — current_scalar_ is a derived telemetry field).
    mutable std::deque<double> scalar_history_;
    mutable double current_scalar_{1.0};

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

    // Daily risk-on stress overlay state. The active flag is telemetry and
    // log-throttling only; the actual weights are recomputed idempotently from
    // pre_market_stress_weights_ on each bar.
    bool market_stress_active_{false};

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
    void apply_defensive_sleeve_vol_awareness(
        std::unordered_map<std::string, double>& weights,
        const MonthlyMacroRecord& rec) const;
    void apply_momentum_tilt(std::unordered_map<std::string, double>& weights) const;
    void apply_homebuilder_tilt(std::unordered_map<std::string, double>& weights,
                                const MonthlyMacroRecord& rec) const;
    void apply_breakout_filter(std::unordered_map<std::string, double>& weights) const;
    void normalize_weights(std::unordered_map<std::string, double>& weights) const;
    void apply_portfolio_vol_target(
        std::unordered_map<std::string, double>& weights) const;
    bool apply_market_stress_throttle(
        std::unordered_map<std::string, double>& weights) const;
    bool is_market_stress_active() const;
    void refresh_market_stress_overlay();

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

    // R1: σ on the un-scaled return series — divide each day's realized
    // return by the scalar that was in effect that day, then compute stdev
    // over the trailing `window`. Falls back to the scaled stdev when
    // scalar_history_ is shorter than window (early backtest warmup).
    double compute_unscaled_return_stdev(int window) const;

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
