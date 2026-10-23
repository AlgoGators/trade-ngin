// include/trade_ngin/strategy/bpgv_rotation.hpp
#pragma once

#include <deque>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/macro_csv_loader.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

/**
 * @brief Tolerance-band rebalancing (Masters 2003), applied in get_target_positions().
 *
 * A held name is re-sized only when its weight drifts more than drift_abs_trigger (absolute)
 * or drift_rel_trigger (relative to target) from the target, and then only halfway back.
 */
struct RebalanceConfig {
    double drift_abs_trigger{0.010};
    double drift_rel_trigger{0.25};
    bool halfway_rule{true};                 // false: trade all the way to target
    bool exit_fully_on_zero_target{true};    // a zero target always exits in full
    bool enter_fully_on_zero_current{true};  // a new name always enters in full

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Momentum on the risk-on sleeve: a 12-month time-series gate (zero a name whose
 *        excess return over the risk-free symbol is below -tsm_tolerance), then a
 *        vol-scaled cross-sectional tilt of +/- xsec_tau by rank, floored at
 *        xsec_weight_floor.
 */
struct MomentumConfig {
    bool tsm_gate_enabled{true};
    int tsm_lookback_days{252};
    std::string tsm_risk_free_symbol{"BIL"};
    bool tsm_fail_open_on_short_history{true};
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
 * @brief Breakout filter on the risk-on sleeve: a name below its sma_window SMA for
 *        confirmation_days closes is zeroed; otherwise its weight is scaled by
 *        (close - SMA) / (atr_k x Wilder ATR) clipped to [0, 1].
 */
struct BreakoutConfig {
    int sma_window{200};
    int atr_window{14};
    double atr_k{2.0};
    int confirmation_days{3};
    bool graded_weighting{true};  // false: 1 above the SMA, 0 below
    bool fail_open_on_short_history{true};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Portfolio volatility target, applied after normalize_weights(): every weight is
 *        scaled by clip(target / sigma, scalar_floor, 1), so sum(weights) <= 1 and the rest
 *        is cash.
 *
 * sigma is the BPGV-GARCH forecast (bpgv_garch x sqrt(12) x multiplier) when
 * use_garch_natural_vol is set and the month has one, else the realized volatility of the
 * strategy's book. With use_max_risk_sigma, sigma is the largest of the included candidates
 * (GARCH, realized book, stress symbol, defensive pair); conditional_max_risk_sigma arms that
 * only while market or breadth stress is active. enable_dynamic_target moves the target from
 * target_annualized_max (GARCH percentile <= pctile_low) to target_annualized_min (>= pctile_high).
 */
struct VolTargetConfig {
    bool enabled{false};
    double target_annualized{0.10};
    int window_days{60};
    double scalar_floor{0.20};
    int min_history_days{60};

    bool enable_dynamic_target{false};
    double target_annualized_min{0.06};
    double target_annualized_max{0.12};
    double pctile_high{75.0};
    double pctile_low{25.0};

    bool use_garch_natural_vol{false};
    double garch_natural_vol_multiplier{1.0};

    bool use_max_risk_sigma{false};
    bool conditional_max_risk_sigma{false};
    bool max_risk_on_market_stress{true};
    bool max_risk_on_breadth_stress{true};
    bool include_garch_sigma{true};
    bool include_realized_portfolio_sigma{true};
    bool include_stress_symbol_sigma{true};
    bool include_defensive_pair_sigma{true};
    std::string stress_vol_symbol{"SPY"};
    int stress_vol_window_days{21};
    int defensive_pair_vol_window_days{60};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Breadth permission: the share of symbols passing a 12-month excess-momentum and a
 *        200-day trend test scales the risk-on sleeve below full_risk_breadth (floored at
 *        risk_on_floor); the released weight goes to splice_symbol.
 */
struct BreadthRiskConfig {
    bool enabled{false};
    std::vector<std::string> symbols;  // empty = risk_on_symbols
    int momentum_lookback_days{252};
    int sma_window{200};
    std::string risk_free_symbol{"BIL"};
    bool require_positive_excess{true};
    bool require_above_sma{true};
    double full_risk_breadth{0.70};
    double stress_breadth{0.50};  // at or below: breadth stress (arms max-risk sigma)
    double risk_on_floor{0.25};
    std::string splice_symbol{"BIL"};
    int min_history_days{252};
    bool fail_open_on_short_history{true};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Defensive sleeve: inverse-vol weights within the risk-off pair, then the pair is
 *        scaled down when its joint realized vol exceeds the target and the slack goes to
 *        splice_symbol. With garch_aware_enabled the target moves between low_sigma and
 *        high_sigma with the month's GARCH percentile.
 */
struct DefensiveSleeveConfig {
    bool vol_aware_enabled{false};
    double pair_vol_target_annualized{0.10};
    int vol_window_days{60};
    bool use_inverse_vol_pair{true};
    std::string splice_symbol{"BIL"};
    double max_splice_fraction{0.80};  // never splice more than this share of the pair
    int min_history_days{60};

    bool garch_aware_enabled{false};
    double pair_vol_target_high_sigma{0.06};
    double pair_vol_target_low_sigma{0.12};
    double garch_pctile_high{75.0};
    double garch_pctile_low{25.0};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Daily market-stress throttle: while stress_symbol is down at least drawdown_trigger
 *        over drawdown_lookback_days AND below its sma_window SMA, the risk-on sleeve is
 *        scaled by risk_on_scale (the rest is cash). Recomputed from the rebalance weights
 *        each day, so it is reversible.
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
 * @brief Permit-growth tilt: trailing single-family permit growth over lookback_months,
 *        divided by growth_scale and clipped to [-1, 1], shifts the risk-off share by up to
 *        max_risk_off_shift.
 */
struct BPGGrowthSignalConfig {
    bool enabled{false};
    int lookback_months{3};
    int min_history_months{2};
    bool use_sfh_bpg{true};
    double growth_scale{0.05};
    double max_risk_off_shift{0.10};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Housing cap: when the GARCH percentile is at least garch_percentile_trigger and the
 *        growth signal is at most growth_signal_trigger, the housing symbols are scaled by
 *        cap_scale and the released weight goes to splice_symbol.
 */
struct HousingExposureConfig {
    bool enabled{false};
    std::vector<std::string> symbols{"XHB", "IYR", "EQR"};
    double garch_percentile_trigger{75.0};
    double growth_signal_trigger{0.0};
    double cap_scale{0.60};
    std::string splice_symbol{"BIL"};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Duration guard: when duration_symbol's trailing return is negative and it is below
 *        its SMA, its weight is scaled by guard_scale and the rest goes to splice_symbol.
 */
struct DurationDefenseConfig {
    bool enabled{false};
    std::string duration_symbol{"TLT"};
    std::string splice_symbol{"BIL"};
    int trend_lookback_days{126};
    int sma_window{200};
    double guard_scale{0.50};
    bool require_negative_return{true};
    bool require_below_sma{true};

    void from_json(const nlohmann::json& j);
};

/**
 * @brief Configuration for the BPGV macro regime rotation strategy.
 */
struct BPGVRotationConfig {
    std::string macro_csv_path{"data/macro/bpgv_regime.csv"};

    // Rebalance on the first day >= this, on the previous month's macro record.
    int rebalance_day_of_month{18};

    // risk_off share = base_risk_off_min + base_risk_off_range x (regime_score + 1) / 2
    double base_risk_off_min{0.05};
    double base_risk_off_range{0.40};

    // Months flagged strong_risk_on scale risk-on up and risk-off down by these.
    double strong_risk_on_equity_boost{0.12};
    double strong_risk_on_bond_reduction{0.20};

    // homebuilder_symbol is scaled up (permit growth > 0) or down by this.
    double homebuilder_tilt_scale{0.20};
    std::string homebuilder_symbol{"XHB"};

    std::vector<std::string> risk_on_symbols{"SPY", "QQQ", "XLK", "SMH", "IWM", "XHB", "IYR", "EQR"};
    std::vector<std::string> risk_off_symbols{"TLT", "GLD"};
    // No base weight; they take the weight the overlays splice out, and the risk-free
    // symbol's history.
    std::vector<std::string> cash_symbols{"BIL", "DBMF"};

    RebalanceConfig rebalance;
    MomentumConfig momentum;
    BreakoutConfig breakout;
    VolTargetConfig vol_target;
    DefensiveSleeveConfig defensive_sleeve;
    MarketStressConfig market_stress;
    BreadthRiskConfig breadth_risk;
    BPGGrowthSignalConfig bpg_growth_signal;
    HousingExposureConfig housing_exposure;
    DurationDefenseConfig duration_defense;

    bool allow_fractional_shares{true};

    // The "config" block this struct was parsed from (empty when built in code), recorded by
    // to_json() so every effective knob is stored without a serializer per sub-config.
    nlohmann::json source_json;

    /// One parser for every runner and test: scalars fall back to the struct defaults,
    /// symbol lists replace the defaults when present, nested blocks go through their own
    /// from_json. Unknown keys are ignored.
    static BPGVRotationConfig from_json(const nlohmann::json& cfg);

    /// Top-level scalars and symbol lists plus the source block under "as_configured".
    nlohmann::json to_json() const;

    /// Leading bars every filter needs before it is armed: the longest of the momentum,
    /// breakout and breadth windows.
    int required_warmup_days() const;
};

/// One symbol's leg of a rebalance, as notional (shares x price) at the sizing price.
struct ExposureLeg {
    double held_notional{0.0};    ///< what the book holds now
    double target_notional{0.0};  ///< what the tolerance-band rule asks for
};

/**
 * @brief The fraction of each exposure-adding leg's increment that the book's cash can fund.
 *
 * A long-only cash book cannot spend more than it has: NAV less what it keeps. A leg adds
 * exposure when its target is above its holding; every other leg (held, trimmed, exited) keeps
 * its target. Returns s in [0, 1] such that
 *   sum over adding legs of (held + s x (target - held)) + sum over the others of target <= nav,
 * i.e. 1 when everything fits and the largest s that fits otherwise. Returns 0 -- nothing new
 * is bought -- when the kept part already reaches nav, when nav <= 0, or when any input is not
 * finite; never more than 1. Pure, so the cap is testable apart from the strategy's state.
 */
double exposure_increment_scale(const std::vector<ExposureLeg>& legs, double nav);

/**
 * @brief BPGV Macro Regime Rotation Strategy
 *
 * Rotates a long-only ETF book between risk-on and risk-off sleeves on a monthly macro
 * regime score built from building-permit growth volatility (data/macro/bpgv_regime.csv).
 * Each month's weights go through the overlays in the order execute_rebalance() lists; the
 * market-stress throttle and the volatility target are refreshed daily. Targets are sized
 * to the prior-close NAV (on_portfolio_value) and capped at it (exposure_increment_scale).
 */
class BPGVRotationStrategy : public BaseStrategy {
public:
    BPGVRotationStrategy(std::string id, StrategyConfig config, BPGVRotationConfig bpgv_config,
                         std::shared_ptr<PostgresDatabase> db,
                         std::shared_ptr<InstrumentRegistry> registry = nullptr);

    Result<void> initialize() override;
    Result<void> on_data(const std::vector<Bar>& data) override;

    // Targets are computed on demand from current_weights_ and live prices; positions_ is
    // owned by on_execution fills.
    std::unordered_map<std::string, Position> get_target_positions() const override;

    std::unordered_map<std::string, std::vector<double>> get_price_history() const override;

    /// Monthly rebalances performed so far (a month rebalances once).
    int get_rebalance_count() const { return rebalance_count_; }

    /// UTC calendar parts of a bar timestamp (bars are UTC-midnight on every path).
    struct DateParts {
        int year;
        int month;
        int day;
    };
    static DateParts extract_date(const Timestamp& ts);

    /// Position-sizing base: the portfolio NAV at the previous close, pushed in by the
    /// backtest coordinator before on_data. Until the first push, sizing uses
    /// config_.capital_allocation.
    void on_portfolio_value(double prior_close_nav) override {
        if (prior_close_nav > 0.0) sizing_equity_ = prior_close_nav;
    }

    /// Leading bars the backtest coordinator treats as warmup.
    int required_warmup_days() const override { return bpgv_config_.required_warmup_days(); }

    /// Current target weights (symbol -> weight).
    const std::unordered_map<std::string, double>& get_current_weights() const {
        return current_weights_;
    }

protected:
    Result<void> validate_config() const override;

private:
    BPGVRotationConfig bpgv_config_;
    std::shared_ptr<InstrumentRegistry> registry_;

    std::vector<MonthlyMacroRecord> macro_data_;

    struct SymbolState {
        std::deque<double> price_history;  // close
        std::deque<double> high_history;
        std::deque<double> low_history;
        double current_price{0.0};
        Timestamp last_update;
    };
    std::unordered_map<std::string, SymbolState> symbol_state_;

    std::unordered_map<std::string, double> current_weights_;
    // The rebalance weights before the daily volatility target and market-stress throttle,
    // which are re-applied from them each day.
    std::unordered_map<std::string, double> pre_vol_target_weights_;
    std::unordered_map<std::string, double> pre_market_stress_weights_;

    // Value of the strategy's holdings, the series the realized-volatility estimate reads.
    double portfolio_value_{0.0};
    std::deque<double> portfolio_value_history_;

    double sizing_equity_{0.0};  // 0 until on_portfolio_value: size to capital_allocation

    // Refreshed from the macro record at each rebalance.
    double current_vol_target_{0.10};
    double current_garch_sigma_natural_{0.0};  // 0: no GARCH forecast that month
    double current_bpgv_garch_percentile_{0.0};

    // Breadth of the last compute_breadth_score() call; mutable because the const overlays
    // compute it.
    mutable double current_breadth_score_{1.0};
    mutable bool current_breadth_valid_{false};

    int last_rebalance_year_{0};
    int last_rebalance_month_{0};
    int rebalance_count_{0};
    bool market_stress_active_{false};  // telemetry and log throttling only

    void update_price_histories(const std::vector<Bar>& bars);
    // One trading day's bars. on_data groups its input by UTC day and calls this per group,
    // so a single-day vector and a multi-day window take the same per-day decisions.
    Result<void> on_day(const std::vector<Bar>& day_bars);
    bool should_rebalance(int year, int month, int day) const;
    void execute_rebalance(int year, int month);

    // Weight pipeline, in execute_rebalance() order.
    std::unordered_map<std::string, double> calculate_base_weights(
        const MonthlyMacroRecord& rec) const;
    void apply_defensive_sleeve_vol_awareness(std::unordered_map<std::string, double>& weights,
                                              const MonthlyMacroRecord& rec) const;
    void apply_momentum_tilt(std::unordered_map<std::string, double>& weights) const;
    void apply_homebuilder_tilt(std::unordered_map<std::string, double>& weights,
                                const MonthlyMacroRecord& rec) const;
    void apply_housing_exposure_cap(std::unordered_map<std::string, double>& weights,
                                    const MonthlyMacroRecord& rec) const;
    void apply_duration_defense_guard(std::unordered_map<std::string, double>& weights) const;
    void apply_breakout_filter(std::unordered_map<std::string, double>& weights) const;
    void apply_breadth_risk_permission(std::unordered_map<std::string, double>& weights) const;
    void normalize_weights(std::unordered_map<std::string, double>& weights) const;
    void apply_portfolio_vol_target(std::unordered_map<std::string, double>& weights) const;
    bool apply_market_stress_throttle(std::unordered_map<std::string, double>& weights) const;

    bool is_market_stress_active() const;
    double compute_breadth_score() const;
    bool is_breadth_stress_active() const;
    bool is_confirmed_risk_stress_active() const;
    void refresh_market_stress_overlay();

    double calculate_sma(const std::deque<double>& prices, int period) const;
    double calculate_trailing_return(const std::deque<double>& prices, int days) const;
    double compute_portfolio_value() const;
    void update_positions_from_weights();
    void trim_price_history(SymbolState& state) const;
    double compute_daily_return_stdev(int window) const;
    double compute_symbol_vol(const std::deque<double>& prices, int window) const;
    double compute_bpg_growth_signal(const MonthlyMacroRecord& rec) const;
    double compute_weighted_bucket_vol(const std::vector<std::string>& symbols,
                                       const std::unordered_map<std::string, double>& weights,
                                       int window) const;
    // Wilder (1978) ATR over the whole history: a 1/period seed, then recursive smoothing.
    static double wilder_atr(const std::deque<double>& high, const std::deque<double>& low,
                             const std::deque<double>& close, int period);
    double breakout_score(const std::deque<double>& high, const std::deque<double>& low,
                          const std::deque<double>& close) const;

    // Kept at 520 bars: wilder_atr smooths over the whole history, so its value depends on
    // the history length.
    static constexpr size_t MAX_PRICE_HISTORY = 520;
};

}  // namespace trade_ngin
