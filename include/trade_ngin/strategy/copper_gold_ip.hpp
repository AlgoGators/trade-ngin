// include/trade_ngin/strategy/copper_gold_ip.hpp
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/daily_macro_csv_loader.hpp"
#include "trade_ngin/instruments/instrument_registry.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"

namespace trade_ngin {

// ============================================================
// Enums
// ============================================================
enum class MacroTilt { RISK_ON, RISK_OFF, NEUTRAL };
enum class EconRegime { GROWTH_POSITIVE, GROWTH_NEGATIVE, INFLATION_SHOCK, LIQUIDITY_SHOCK, NEUTRAL };
enum class DXYFilter { CONFIRMED, SUSPECT, NEUTRAL };

// ============================================================
// Configuration
// ============================================================
struct CopperGoldIPConfig {
    // Data paths
    std::string macro_csv_path{"data/macro/copper_gold_daily.csv"};

    // Layer 1: Core Cu/Au signal
    int roc_window{20};
    int ma_fast{10};
    int ma_slow{50};
    int zscore_window{120};
    double zscore_threshold{0.5};
    double w1{0.33};
    double w2{0.33};
    double w3{0.34};
    int min_holding_period{5};

    // Layer 2: Regime classification
    int spx_momentum_lookback{60};
    int breakeven_lookback{20};
    int liquidity_zscore_window{60};
    double liquidity_threshold{-1.5};
    double inflation_shock_threshold{0.15};    // 15bps breakeven change over lookback
    double growth_positive_threshold{0.5};     // SPX momentum % for GROWTH_POSITIVE
    double growth_negative_threshold{-0.5};    // SPX momentum % for GROWTH_NEGATIVE

    // Layer 3: DXY filter
    int dxy_momentum_lookback{20};
    double dxy_momentum_threshold{0.03};

    // China filter
    int china_cli_avg_window{65};
    double china_cli_threshold{-2.0};

    // Correlation spike
    int correlation_window{20};
    double correlation_threshold{0.70};

    // Risk management
    double leverage_target{2.0};
    double max_margin_utilization{0.50};
    double drawdown_warning_pct{0.10};
    double drawdown_stop_pct{0.15};

    // Position limits
    double max_single_equity_notional{0.20};
    double max_single_commodity_notional{0.15};
    double max_total_equity_notional{0.35};
    double max_total_commodity_notional{0.40};

    // Instrument universe (use .v.0 continuous contract symbols)
    std::vector<std::string> futures_symbols{
        "HG.v.0", "GC.v.0", "CL.v.0", "SI.v.0", "ZN.v.0", "UB.v.0",
        "6J.v.0", "MES.v.0", "MNQ.v.0"};

    // --- Improvement feature flags (all default OFF to preserve baseline) ---

    // 1. Ensemble ROC lookbacks
    bool use_ensemble_roc{false};
    std::vector<int> roc_windows{10, 20, 60, 120};

    // 2. Gold/Silver ratio filter
    bool use_gsr_filter{false};
    double gsr_fear_threshold{80.0};
    double gsr_greed_threshold{65.0};
    double gsr_conflict_scale{0.75};

    // 3. Inverse-volatility asset class weights
    bool use_inverse_vol_weights{false};
    int vol_weight_lookback{63};
    double min_asset_class_weight{0.10};
    double max_asset_class_weight{0.50};

    // 4. Conditional volatility targeting
    bool use_vol_targeting{false};
    double target_portfolio_vol{0.15};
    int vol_target_lookback{63};
    double min_leverage{0.5};
    double max_leverage{3.0};

    // 5. Cross-asset momentum confirmation
    bool use_momentum_confirmation{false};
    std::vector<int> momentum_lookbacks{21, 63, 252};
    double momentum_disagreement_scale{0.5};

    // 6. Carry signal (approximate)
    bool use_carry_modifier{false};
    int carry_lookback{63};
    double carry_disagreement_scale{0.65};
};

// ============================================================
// Contract specification for position sizing
// ============================================================
struct FuturesContractSpec {
    double margin{5000.0};
    double notional{100000.0};
    double tick_size{0.01};
    double tick_value{10.0};
    double commission_rt{2.50};
    double spread_ticks{1.0};
    double slippage_ticks{0.5};

    double point_value() const { return tick_value / tick_size; }
    double total_cost_rt() const {
        return commission_rt + (spread_ticks * tick_value) + (2.0 * slippage_ticks * tick_value);
    }
};

// ============================================================
// Strategy
// ============================================================
class CopperGoldIPStrategy : public BaseStrategy {
public:
    CopperGoldIPStrategy(std::string id, StrategyConfig config, CopperGoldIPConfig cg_config,
                         std::shared_ptr<PostgresDatabase> db,
                         std::shared_ptr<InstrumentRegistry> registry = nullptr);

    Result<void> initialize() override;
    Result<void> on_data(const std::vector<Bar>& data) override;

    std::unordered_map<std::string, std::vector<double>> get_price_history() const override;

    MacroTilt get_current_tilt() const { return signal_state_.current_tilt; }
    EconRegime get_current_regime() const { return signal_state_.current_econ_regime; }
    double get_current_equity() const { return signal_state_.equity; }

protected:
    Result<void> validate_config() const override;

private:
    CopperGoldIPConfig cg_config_;
    std::shared_ptr<InstrumentRegistry> registry_;

    // Pre-loaded daily macro data
    std::vector<DailyMacroRecord> macro_data_;

    // Per-symbol futures state
    struct FuturesState {
        std::deque<double> price_history;
        double current_price{0.0};
        Timestamp last_update;
    };
    std::unordered_map<std::string, FuturesState> futures_state_;

    // Rolling macro time series (extracted from macro records each day)
    struct MacroState {
        std::deque<double> dxy_history;
        std::deque<double> vix_history;
        std::deque<double> hy_spread_history;
        std::deque<double> breakeven_history;
        std::deque<double> yield_10y_history;
        std::deque<double> tips_10y_history;
        std::deque<double> spx_history;
        std::deque<double> fed_bs_history;
        std::deque<double> china_cli_history;
    };
    MacroState macro_state_;

    // Cu/Au ratio history
    std::deque<double> cu_au_ratio_history_;

    // Signal state persisting across bars
    struct SignalState {
        MacroTilt current_tilt{MacroTilt::NEUTRAL};
        MacroTilt pending_tilt{MacroTilt::NEUTRAL};
        int pending_tilt_count{0};

        EconRegime current_econ_regime{EconRegime::NEUTRAL};
        EconRegime pending_econ_regime{EconRegime::NEUTRAL};
        int pending_regime_count{0};

        DXYFilter prev_dxy_filter{DXYFilter::NEUTRAL};

        double equity{0.0};
        double peak_equity{0.0};

        bool drawdown_warning_active{false};
        bool drawdown_stop_active{false};

        int bars_processed{0};
        bool warmup_reset_done{false};
    };
    SignalState signal_state_;

    // Contract specs (hardcoded from strategy doc)
    static const std::unordered_map<std::string, FuturesContractSpec> CONTRACT_SPECS;
    static const std::unordered_map<std::string, double> ASSET_CLASS_WEIGHTS;

    // Improvement 3: Cached dynamic weights
    std::unordered_map<std::string, double> dynamic_weights_;

    // Improvement 4: Portfolio return history for vol targeting
    std::deque<double> portfolio_return_history_;
    double prev_portfolio_value_{0.0};

    // --- Layer methods ---
    double compute_layer1_composite();
    MacroTilt apply_holding_period(MacroTilt raw_tilt);
    EconRegime classify_econ_regime(double growth, double inflation, double regime_liquidity);
    DXYFilter compute_dxy_filter(double dxy_momentum, MacroTilt tilt);
    double compute_size_multiplier(MacroTilt tilt, EconRegime regime, DXYFilter dxy_filter,
                                   double supp_liquidity, bool corr_spike);
    double compute_china_adjustment();
    bool compute_safe_haven_override();
    bool compute_correlation_spike();

    // --- Improvement methods ---
    double compute_gold_silver_ratio() const;                                    // Improvement 2
    double compute_gsr_modifier(MacroTilt tilt) const;                           // Improvement 2
    std::unordered_map<std::string, double> compute_dynamic_weights() const;     // Improvement 3
    double compute_portfolio_vol() const;                                        // Improvement 4
    double compute_dynamic_leverage() const;                                     // Improvement 4
    double compute_momentum_agreement(const std::string& sym, double dir) const; // Improvement 5
    double compute_approximate_carry(const std::string& sym) const;              // Improvement 6
    double compute_carry_modifier(const std::string& sym, double dir) const;     // Improvement 6

    // --- Position sizing ---
    std::unordered_map<std::string, double> compute_target_positions(
        MacroTilt tilt, EconRegime regime, double size_mult,
        double china_adj, bool skip_gold_short, double composite);
    double contracts_for(const std::string& sym, double direction, double size_mult,
                         double vol_adj = 1.0);
    void apply_position_limits(std::unordered_map<std::string, double>& positions);
    bool should_rebalance(bool is_friday, bool tilt_changed, bool regime_changed,
                          bool filter_triggered, bool stop_triggered,
                          const std::unordered_map<std::string, double>& positions);

    // --- Rolling statistics helpers ---
    double compute_sma(const std::deque<double>& series, int period) const;
    double compute_std(const std::deque<double>& series, int period) const;
    double compute_zscore_val(const std::deque<double>& series, int window) const;
    double compute_roc(const std::deque<double>& series, int lookback) const;
    double compute_percentile(const std::deque<double>& series, int window, double current) const;
    double compute_momentum(const std::deque<double>& series, int lookback) const;

    // --- Utility ---
    void update_price_histories(const std::vector<Bar>& bars);
    void update_macro_state(const DailyMacroRecord& rec);
    void trim_deque(std::deque<double>& d) const;
    double compute_si_vol_adjustment() const;

    struct DateParts { int year; int month; int day; };
    static DateParts extract_date(const Timestamp& ts);
    static bool is_friday(const Timestamp& ts);
    static std::string asset_class_of(const std::string& sym);

    static constexpr size_t MAX_HISTORY = 300;
};

}  // namespace trade_ngin
