// include/trade_ngin/strategy/bpgv_rotation.hpp
#pragma once

#include <algorithm>
#include <deque>
#include <memory>
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

    // Per-asset extreme allocation weights (risk-on extreme and risk-off extreme)
    // If empty, equal weight within each bucket is used.
    std::unordered_map<std::string, double> risk_on_extreme_weights;
    std::unordered_map<std::string, double> risk_off_extreme_weights;

    // Crash override weights (symbol -> weight). If empty, uses default split.
    std::unordered_map<std::string, double> crash_weights;

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
        int momentum = bpgv_config_.momentum_lookback_days;
        int breakout = bpgv_config_.breakout_sma_window;
        int crash = bpgv_config_.crash_lookback_days + 10;
        return std::max({momentum, breakout, crash});
    }

protected:
    Result<void> validate_config() const override;

private:
    BPGVRotationConfig bpgv_config_;
    std::shared_ptr<InstrumentRegistry> registry_;

    // Pre-loaded macro regime data from CSV
    std::vector<MonthlyMacroRecord> macro_data_;

    // Per-symbol daily state
    struct SymbolState {
        std::deque<double> price_history;
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

    struct DateParts {
        int year;
        int month;
        int day;
    };
    static DateParts extract_date(const Timestamp& ts);

    static constexpr size_t MAX_PRICE_HISTORY = 300;
};

}  // namespace trade_ngin
