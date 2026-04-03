// src/strategy/copper_gold_ip.cpp
#include "trade_ngin/strategy/copper_gold_ip.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace trade_ngin {

// ============================================================
// Contract specs from strategy document (lines 449-458)
// ============================================================
const std::unordered_map<std::string, FuturesContractSpec> CopperGoldIPStrategy::CONTRACT_SPECS = {
    {"HG",  {6000.0,  110000.0, 0.0005,   12.50,  2.50,  0.5, 0.5}},
    {"GC",  {11000.0, 200000.0, 0.10,     10.00,  2.50,  1.0, 0.5}},
    {"CL",  {7000.0,   75000.0, 0.01,     10.00,  2.50,  1.0, 1.0}},
    {"SI",  {10000.0, 150000.0, 0.005,    25.00,  2.50,  1.0, 1.0}},
    {"ZN",  {2500.0,  110000.0, 0.015625, 15.625, 1.50,  0.5, 0.5}},
    {"UB",  {9000.0,  130000.0, 0.03125,  31.25,  2.50,  0.0, 0.0}},
    {"MNQ", {2000.0,   40000.0, 0.25,      0.50,  0.50,  1.0, 0.5}},
};

const std::unordered_map<std::string, double> CopperGoldIPStrategy::ASSET_CLASS_WEIGHTS = {
    {"equity_index", 0.30},
    {"commodities",  0.35},
    {"fixed_income", 0.25},
};

// ============================================================
// Constructor
// ============================================================
CopperGoldIPStrategy::CopperGoldIPStrategy(
    std::string id, StrategyConfig config, CopperGoldIPConfig cg_config,
    std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      cg_config_(std::move(cg_config)),
      registry_(registry) {
    Logger::register_component("CopperGoldIP");

    metadata_.name = "Copper-Gold IP Macro Regime Strategy";
    metadata_.description = "Macro-driven regime strategy using copper/gold ratio as risk indicator";
}

// ============================================================
// Validation
// ============================================================
Result<void> CopperGoldIPStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error()) return result;

    if (cg_config_.min_holding_period < 1) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Minimum holding period must be >= 1",
                                "CopperGoldIPStrategy");
    }
    if (cg_config_.futures_symbols.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Futures symbols list cannot be empty",
                                "CopperGoldIPStrategy");
    }
    return Result<void>();
}

// ============================================================
// Initialize
// ============================================================
Result<void> CopperGoldIPStrategy::initialize() {
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        ERROR("Base strategy initialization failed: " + std::string(base_result.error()->what()));
        return base_result;
    }

    set_pnl_accounting_method(PnLAccountingMethod::REALIZED_ONLY);
    INFO("Copper-Gold IP strategy using REALIZED_ONLY PnL accounting (futures mark-to-market)");

    // Load daily macro data
    auto macro_result = DailyMacroCSVLoader::load(cg_config_.macro_csv_path);
    if (macro_result.is_error()) {
        ERROR("Failed to load daily macro CSV: " + std::string(macro_result.error()->what()));
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                "Cannot initialize CopperGoldIP without macro data: " +
                                    std::string(macro_result.error()->what()),
                                "CopperGoldIPStrategy");
    }
    macro_data_ = macro_result.value();
    INFO("Loaded " + std::to_string(macro_data_.size()) + " daily macro records");

    // Initialize per-symbol state and positions
    // Include HG (needed for ratio even though not directly traded in V5 simplified set)
    auto all_symbols = cg_config_.futures_symbols;
    if (std::find(all_symbols.begin(), all_symbols.end(), "HG") == all_symbols.end()) {
        all_symbols.push_back("HG");
    }

    for (const auto& symbol : all_symbols) {
        futures_state_[symbol] = FuturesState{};

        Position pos;
        pos.symbol = symbol;
        pos.quantity = 0.0;
        pos.average_price = 0.0;
        pos.last_update = std::chrono::system_clock::now();
        positions_[symbol] = pos;
    }

    signal_state_.equity = config_.capital_allocation;
    signal_state_.peak_equity = config_.capital_allocation;

    INFO("Copper-Gold IP strategy initialized for " + std::to_string(all_symbols.size()) + " symbols");
    return Result<void>();
}

// ============================================================
// on_data — daily heartbeat
// ============================================================
Result<void> CopperGoldIPStrategy::on_data(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "Strategy is not in running state",
                                "CopperGoldIPStrategy");
    }
    if (data.empty()) return Result<void>();

    try {
        // 1. Update futures price histories
        update_price_histories(data);

        // 2. Compute Cu/Au ratio
        auto hg_it = futures_state_.find("HG");
        auto gc_it = futures_state_.find("GC");
        if (hg_it == futures_state_.end() || gc_it == futures_state_.end() ||
            hg_it->second.current_price <= 0.0 || gc_it->second.current_price <= 0.0) {
            return Result<void>();  // Not enough data yet
        }

        double cu_notional = hg_it->second.current_price * 25000.0;
        double au_notional = gc_it->second.current_price * 100.0;
        double ratio = cu_notional / au_notional;
        cu_au_ratio_history_.push_back(ratio);
        trim_deque(cu_au_ratio_history_);

        // 3. Extract date and look up macro record
        auto date = extract_date(data.front().timestamp);
        auto macro_rec = DailyMacroCSVLoader::find_record_before(
            macro_data_, date.year, date.month, date.day);

        if (macro_rec.has_value()) {
            update_macro_state(*macro_rec);
        }

        // Need minimum history for signal generation
        if (cu_au_ratio_history_.size() < static_cast<size_t>(cg_config_.zscore_window)) {
            return Result<void>();
        }

        // ============================================================
        // Layer 1: Core Cu/Au Signal
        // ============================================================
        double composite = compute_layer1_composite();
        MacroTilt raw_tilt = MacroTilt::NEUTRAL;
        if (!std::isnan(composite)) {
            raw_tilt = (composite > 0.0) ? MacroTilt::RISK_ON : MacroTilt::RISK_OFF;
        }
        MacroTilt macro_tilt = apply_holding_period(raw_tilt);
        MacroTilt prev_tilt = signal_state_.current_tilt;
        bool tilt_changed = (macro_tilt != prev_tilt && prev_tilt != MacroTilt::NEUTRAL);

        signal_state_.current_tilt = macro_tilt;

        // ============================================================
        // Layer 2: Regime Classification
        // ============================================================
        double growth = 0.0;
        if (macro_state_.spx_history.size() >= static_cast<size_t>(cg_config_.spx_momentum_lookback)) {
            growth = compute_momentum(macro_state_.spx_history, cg_config_.spx_momentum_lookback) * 100.0;
        }

        double inflation = 0.0;
        if (macro_state_.breakeven_history.size() >= static_cast<size_t>(cg_config_.breakeven_lookback)) {
            size_t sz = macro_state_.breakeven_history.size();
            inflation = macro_state_.breakeven_history.back() -
                        macro_state_.breakeven_history[sz - cg_config_.breakeven_lookback];
        }

        // Regime liquidity: -1 * (DXY_zscore + HY_zscore + VIX_zscore) / 3
        double dxy_z = compute_zscore_val(macro_state_.dxy_history, cg_config_.liquidity_zscore_window);
        double hy_z = compute_zscore_val(macro_state_.hy_spread_history, cg_config_.liquidity_zscore_window);
        double vix_z = compute_zscore_val(macro_state_.vix_history, cg_config_.liquidity_zscore_window);
        double regime_liquidity = -1.0 * (dxy_z + hy_z + vix_z) / 3.0;

        EconRegime regime = classify_econ_regime(growth, inflation, regime_liquidity);

        // Track regime changes (3-day confirmation)
        EconRegime prev_regime = signal_state_.current_econ_regime;
        if (regime != prev_regime) {
            if (regime == signal_state_.pending_econ_regime) {
                signal_state_.pending_regime_count++;
            } else {
                signal_state_.pending_econ_regime = regime;
                signal_state_.pending_regime_count = 1;
            }
        } else {
            signal_state_.pending_econ_regime = regime;
            signal_state_.pending_regime_count = 0;
        }
        bool regime_changed = (signal_state_.pending_regime_count >= 3);
        if (regime_changed) {
            signal_state_.current_econ_regime = regime;
        }
        regime = signal_state_.current_econ_regime;

        // ============================================================
        // Layer 3: DXY Filter
        // ============================================================
        double dxy_mom = 0.0;
        if (macro_state_.dxy_history.size() >= static_cast<size_t>(cg_config_.dxy_momentum_lookback)) {
            dxy_mom = compute_momentum(macro_state_.dxy_history, cg_config_.dxy_momentum_lookback);
        }

        DXYFilter dxy_filter = compute_dxy_filter(dxy_mom, macro_tilt);
        DXYFilter prev_dxy_filter = signal_state_.prev_dxy_filter;
        bool filter_triggered = (dxy_filter == DXYFilter::SUSPECT && prev_dxy_filter != DXYFilter::SUSPECT);
        signal_state_.prev_dxy_filter = dxy_filter;

        // ============================================================
        // Supplementary liquidity proxy
        // ============================================================
        double vix_percentile = compute_percentile(macro_state_.vix_history, 60,
            macro_state_.vix_history.empty() ? 0.0 : macro_state_.vix_history.back());
        double fed_bs_yoy = 0.0;
        if (macro_state_.fed_bs_history.size() >= 252) {
            size_t sz = macro_state_.fed_bs_history.size();
            double old_val = macro_state_.fed_bs_history[sz - 252];
            if (old_val > 0.0) {
                fed_bs_yoy = (macro_state_.fed_bs_history.back() / old_val) - 1.0;
            }
        }
        double vix_component = -1.0 * (vix_percentile * 6.0 - 3.0);
        double hy_component = -1.0 * hy_z;
        double fbs_component = fed_bs_yoy * 10.0;
        double supp_liquidity = (vix_component + hy_component + fbs_component) / 3.0;

        // ============================================================
        // Safe-haven override and China filter
        // ============================================================
        bool skip_gold_short = compute_safe_haven_override();
        double china_adj = compute_china_adjustment();
        bool corr_spike = compute_correlation_spike();

        // ============================================================
        // PnL & Equity Update
        // ============================================================
        // Mark-to-market: compute daily PnL from position changes
        double daily_pnl = 0.0;
        for (const auto& [sym, pos] : positions_) {
            double qty = pos.quantity.as_double();
            if (std::abs(qty) < 1e-9) continue;

            auto fs_it = futures_state_.find(sym);
            if (fs_it == futures_state_.end() || fs_it->second.price_history.size() < 2) continue;

            size_t sz = fs_it->second.price_history.size();
            double prev_price = fs_it->second.price_history[sz - 2];
            double curr_price = fs_it->second.price_history[sz - 1];

            auto spec_it = CONTRACT_SPECS.find(sym);
            double pv = (spec_it != CONTRACT_SPECS.end()) ? spec_it->second.point_value() : 1.0;

            daily_pnl += qty * (curr_price - prev_price) * pv;
        }

        signal_state_.equity += daily_pnl;
        if (signal_state_.equity > signal_state_.peak_equity) {
            signal_state_.peak_equity = signal_state_.equity;
        }

        // ============================================================
        // Size Multiplier (Layers 2-5 combined)
        // ============================================================
        double drawdown = 0.0;
        if (signal_state_.peak_equity > 0.0) {
            drawdown = (signal_state_.peak_equity - signal_state_.equity) / signal_state_.peak_equity;
        }

        double size_mult = compute_size_multiplier(
            macro_tilt, regime, dxy_filter, supp_liquidity, corr_spike, drawdown);

        bool stop_triggered = signal_state_.drawdown_stop_active || signal_state_.drawdown_warning_active;

        // ============================================================
        // Rebalance Decision
        // ============================================================
        bool friday = is_friday(data.front().timestamp);

        // Build current positions map (double)
        std::unordered_map<std::string, double> current_pos;
        for (const auto& [sym, pos] : positions_) {
            current_pos[sym] = pos.quantity.as_double();
        }

        bool do_rebalance = should_rebalance(
            friday, tilt_changed, regime_changed, filter_triggered, stop_triggered, current_pos);

        if (!do_rebalance) {
            // Update position prices for mark-to-market
            for (const auto& bar : data) {
                auto pos_it = positions_.find(bar.symbol);
                if (pos_it != positions_.end()) {
                    pos_it->second.average_price = bar.close;
                    pos_it->second.last_update = bar.timestamp;
                }
            }
            return Result<void>();
        }

        // ============================================================
        // Position Sizing
        // ============================================================
        auto new_positions = compute_target_positions(
            macro_tilt, regime, size_mult, china_adj, skip_gold_short, composite);

        apply_position_limits(new_positions);

        // Apply margin utilization cap
        double total_margin = 0.0;
        for (const auto& [sym, qty] : new_positions) {
            auto spec_it = CONTRACT_SPECS.find(sym);
            double margin = (spec_it != CONTRACT_SPECS.end()) ? spec_it->second.margin : 5000.0;
            total_margin += std::abs(qty) * margin;
        }
        double margin_util = (signal_state_.equity > 0.0) ? total_margin / signal_state_.equity : 0.0;
        if (margin_util > cg_config_.max_margin_utilization && margin_util > 0.0) {
            double scale = cg_config_.max_margin_utilization / margin_util;
            for (auto& [sym, qty] : new_positions) {
                qty = std::floor(qty * scale + 0.5);
            }
        }

        // Deduct transaction costs for position changes
        for (const auto& [sym, new_qty] : new_positions) {
            double old_qty = current_pos.count(sym) ? current_pos.at(sym) : 0.0;
            double qty_change = std::abs(new_qty - old_qty);
            if (qty_change > 0.0) {
                auto spec_it = CONTRACT_SPECS.find(sym);
                double cost = (spec_it != CONTRACT_SPECS.end()) ?
                    spec_it->second.total_cost_rt() * qty_change : 0.0;
                signal_state_.equity -= cost;
            }
        }

        // Update positions
        for (const auto& [sym, qty] : new_positions) {
            auto pos_it = positions_.find(sym);
            if (pos_it != positions_.end()) {
                pos_it->second.quantity = Quantity(qty);
                auto fs_it = futures_state_.find(sym);
                if (fs_it != futures_state_.end() && fs_it->second.current_price > 0.0) {
                    pos_it->second.average_price = Price(fs_it->second.current_price);
                }
            }
        }

        // Update position prices for mark-to-market
        for (const auto& bar : data) {
            auto pos_it = positions_.find(bar.symbol);
            if (pos_it != positions_.end()) {
                pos_it->second.average_price = bar.close;
                pos_it->second.last_update = bar.timestamp;
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error in CopperGoldIPStrategy::on_data: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                "Failed to process market data: " + std::string(e.what()),
                                "CopperGoldIPStrategy");
    }
}

// ============================================================
// Layer 1: Composite signal
// ============================================================
double CopperGoldIPStrategy::compute_layer1_composite() {
    if (cu_au_ratio_history_.size() < static_cast<size_t>(cg_config_.zscore_window)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // ROC(20)
    double roc20 = compute_roc(cu_au_ratio_history_, cg_config_.roc_window);
    double sign_roc = std::isnan(roc20) ? 0.0 : (roc20 > 0.0 ? 1.0 : -1.0);

    // MA crossover: SMA(fast) vs SMA(slow)
    double sma_fast = compute_sma(cu_au_ratio_history_, cg_config_.ma_fast);
    double sma_slow = compute_sma(cu_au_ratio_history_, cg_config_.ma_slow);
    double signal_ma = 0.0;
    if (!std::isnan(sma_fast) && !std::isnan(sma_slow)) {
        signal_ma = (sma_fast > sma_slow) ? 1.0 : -1.0;
    }

    // Z-score
    double sma_z = compute_sma(cu_au_ratio_history_, cg_config_.zscore_window);
    double std_z = compute_std(cu_au_ratio_history_, cg_config_.zscore_window);
    double signal_z = 0.0;
    if (!std::isnan(sma_z) && !std::isnan(std_z) && std_z > 0.0) {
        double zscore = (cu_au_ratio_history_.back() - sma_z) / std_z;
        if (zscore > cg_config_.zscore_threshold) signal_z = 1.0;
        else if (zscore < -cg_config_.zscore_threshold) signal_z = -1.0;
    }

    return cg_config_.w1 * sign_roc + cg_config_.w2 * signal_ma + cg_config_.w3 * signal_z;
}

// ============================================================
// Minimum holding period
// ============================================================
MacroTilt CopperGoldIPStrategy::apply_holding_period(MacroTilt raw_tilt) {
    MacroTilt current = signal_state_.current_tilt;

    if (raw_tilt != current) {
        if (raw_tilt != signal_state_.pending_tilt) {
            signal_state_.pending_tilt = raw_tilt;
            signal_state_.pending_tilt_count = 1;
        } else {
            signal_state_.pending_tilt_count++;
        }
        if (signal_state_.pending_tilt_count >= cg_config_.min_holding_period) {
            signal_state_.pending_tilt_count = 0;
            return signal_state_.pending_tilt;
        }
        return current;
    }

    signal_state_.pending_tilt = raw_tilt;
    signal_state_.pending_tilt_count = 0;
    return current;
}

// ============================================================
// Regime classification
// ============================================================
EconRegime CopperGoldIPStrategy::classify_econ_regime(
    double growth, double inflation, double regime_liquidity) {

    if (regime_liquidity < cg_config_.liquidity_threshold) {
        return EconRegime::LIQUIDITY_SHOCK;
    } else if (inflation > 1.0 && growth < 0.5) {
        return EconRegime::INFLATION_SHOCK;
    } else if (growth > 0.5) {
        return EconRegime::GROWTH_POSITIVE;
    } else if (growth < -0.5) {
        return EconRegime::GROWTH_NEGATIVE;
    }
    return EconRegime::NEUTRAL;
}

// ============================================================
// DXY Filter
// ============================================================
DXYFilter CopperGoldIPStrategy::compute_dxy_filter(double dxy_momentum, MacroTilt tilt) {
    if (dxy_momentum > cg_config_.dxy_momentum_threshold) {
        return (tilt == MacroTilt::RISK_ON) ? DXYFilter::SUSPECT : DXYFilter::CONFIRMED;
    } else if (dxy_momentum < -cg_config_.dxy_momentum_threshold) {
        return (tilt == MacroTilt::RISK_OFF) ? DXYFilter::SUSPECT : DXYFilter::CONFIRMED;
    }
    return DXYFilter::NEUTRAL;
}

// ============================================================
// Size multiplier cascade
// ============================================================
double CopperGoldIPStrategy::compute_size_multiplier(
    MacroTilt tilt, EconRegime regime, DXYFilter dxy_filter,
    double supp_liquidity, bool corr_spike, double drawdown) {

    double size_mult = 1.0;

    // Regime-based adjustment
    if (regime == EconRegime::LIQUIDITY_SHOCK) {
        size_mult = 0.0;
    } else if (tilt == MacroTilt::RISK_OFF && regime == EconRegime::GROWTH_POSITIVE) {
        size_mult = 0.0;
    } else if (tilt == MacroTilt::RISK_ON && regime == EconRegime::GROWTH_NEGATIVE) {
        size_mult = 0.5;
    } else if (regime == EconRegime::NEUTRAL) {
        size_mult = 0.5;
    } else if (regime == EconRegime::INFLATION_SHOCK) {
        size_mult = 0.5;
    }

    // DXY filter
    if (dxy_filter == DXYFilter::SUSPECT) {
        size_mult *= 0.5;
    }

    // Supplementary liquidity
    if (supp_liquidity < cg_config_.liquidity_threshold) {
        size_mult *= 0.25;
    }

    // Correlation spike
    if (corr_spike) {
        size_mult *= 0.5;
    }

    // Drawdown management
    signal_state_.drawdown_stop_active = false;
    signal_state_.drawdown_warning_active = false;

    if (drawdown > cg_config_.drawdown_stop_pct) {
        size_mult = 0.0;
        signal_state_.drawdown_stop_active = true;
    } else if (drawdown > cg_config_.drawdown_warning_pct) {
        size_mult *= 0.5;
        signal_state_.drawdown_warning_active = true;
    }

    return size_mult;
}

// ============================================================
// China filter
// ============================================================
double CopperGoldIPStrategy::compute_china_adjustment() {
    if (macro_state_.china_cli_history.size() < static_cast<size_t>(cg_config_.china_cli_avg_window)) {
        return 1.0;
    }
    double cli_avg = compute_sma(macro_state_.china_cli_history, cg_config_.china_cli_avg_window);
    double current_cli = macro_state_.china_cli_history.back();
    if ((current_cli - cli_avg) < cg_config_.china_cli_threshold) {
        return 0.7;
    }
    return 1.0;
}

// ============================================================
// Safe-haven override (skip gold short during crisis)
// ============================================================
bool CopperGoldIPStrategy::compute_safe_haven_override() {
    if (macro_state_.vix_history.size() < 60 || macro_state_.spx_history.size() < 2) {
        return false;
    }

    auto gc_it = futures_state_.find("GC");
    if (gc_it == futures_state_.end() || gc_it->second.price_history.size() < 2) {
        return false;
    }

    double vix_90 = compute_percentile(macro_state_.vix_history, 60,
        macro_state_.vix_history.back());
    double current_vix = macro_state_.vix_history.back();

    size_t gc_sz = gc_it->second.price_history.size();
    double gold_ret = (gc_it->second.price_history[gc_sz - 1] /
                       gc_it->second.price_history[gc_sz - 2]) - 1.0;

    size_t spx_sz = macro_state_.spx_history.size();
    double eq_ret = (macro_state_.spx_history[spx_sz - 1] /
                     macro_state_.spx_history[spx_sz - 2]) - 1.0;

    // VIX > 90th percentile AND gold up >1.5% AND equities down >1.5%
    return (current_vix > 0.0 && vix_90 > 0.90 && gold_ret > 0.015 && eq_ret < -0.015);
}

// ============================================================
// Correlation spike detection
// ============================================================
bool CopperGoldIPStrategy::compute_correlation_spike() {
    // Compute average pairwise correlation of traded instrument returns
    int window = cg_config_.correlation_window;

    // Collect return series for all symbols with enough history
    std::vector<std::deque<double>> return_series;
    for (const auto& sym : cg_config_.futures_symbols) {
        auto it = futures_state_.find(sym);
        if (it == futures_state_.end() || it->second.price_history.size() < static_cast<size_t>(window + 1)) {
            continue;
        }

        std::deque<double> rets;
        const auto& ph = it->second.price_history;
        for (size_t j = ph.size() - window; j < ph.size(); ++j) {
            if (j > 0 && ph[j - 1] > 0.0) {
                rets.push_back(std::log(ph[j] / ph[j - 1]));
            }
        }
        if (static_cast<int>(rets.size()) >= window - 1) {
            return_series.push_back(rets);
        }
    }

    if (return_series.size() < 2) return false;

    // Average pairwise correlation
    double sum_corr = 0.0;
    int pairs = 0;
    int n = return_series.size();
    for (int a = 0; a < n; ++a) {
        for (int b = a + 1; b < n; ++b) {
            int len = std::min(return_series[a].size(), return_series[b].size());
            if (len < 2) continue;

            double ma = 0.0, mb = 0.0;
            for (int k = 0; k < len; ++k) {
                ma += return_series[a][k];
                mb += return_series[b][k];
            }
            ma /= len;
            mb /= len;

            double cov = 0.0, va = 0.0, vb = 0.0;
            for (int k = 0; k < len; ++k) {
                double da = return_series[a][k] - ma;
                double db = return_series[b][k] - mb;
                cov += da * db;
                va += da * da;
                vb += db * db;
            }
            double denom = std::sqrt(va * vb);
            if (denom > 0.0) {
                sum_corr += cov / denom;
                pairs++;
            }
        }
    }

    double avg_corr = (pairs > 0) ? sum_corr / pairs : 0.0;
    return avg_corr > cg_config_.correlation_threshold;
}

// ============================================================
// Position sizing helpers
// ============================================================
double CopperGoldIPStrategy::contracts_for(
    const std::string& sym, double direction, double size_mult, double vol_adj) {

    if (std::abs(direction) < 1e-9 || std::abs(size_mult) < 1e-9) return 0.0;

    std::string ac = asset_class_of(sym);
    double w = ASSET_CLASS_WEIGHTS.count(ac) ? ASSET_CLASS_WEIGHTS.at(ac) : 0.1;

    double notional_alloc = signal_state_.equity * cg_config_.leverage_target * w;

    auto spec_it = CONTRACT_SPECS.find(sym);
    double notional = (spec_it != CONTRACT_SPECS.end()) ? spec_it->second.notional : 100000.0;

    double raw = (notional_alloc / notional) * size_mult * vol_adj;
    return std::floor(raw * direction + 0.5);
}

double CopperGoldIPStrategy::compute_si_vol_adjustment() const {
    // Silver volatility relative to gold (for position sizing normalization)
    auto gc_it = futures_state_.find("GC");
    auto si_it = futures_state_.find("SI");
    if (gc_it == futures_state_.end() || si_it == futures_state_.end()) return 1.0;

    const auto& gc_ph = gc_it->second.price_history;
    const auto& si_ph = si_it->second.price_history;
    if (gc_ph.size() < 21 || si_ph.size() < 21) return 1.0;

    // Simple ATR proxy: average absolute daily change over 20 days
    auto avg_abs_change = [](const std::deque<double>& ph, int period) -> double {
        double sum = 0.0;
        int count = 0;
        for (size_t i = ph.size() - period; i < ph.size(); ++i) {
            if (i > 0 && ph[i - 1] > 0.0) {
                sum += std::abs(ph[i] - ph[i - 1]);
                count++;
            }
        }
        return (count > 0) ? sum / count : 0.0;
    };

    double gc_atr = avg_abs_change(gc_ph, 20) * 100.0;    // GC: 100 oz/contract
    double si_atr = avg_abs_change(si_ph, 20) * 5000.0;   // SI: 5000 oz/contract

    return (si_atr > 0.0) ? gc_atr / si_atr : 1.0;
}

// ============================================================
// Position calculation
// ============================================================
std::unordered_map<std::string, double> CopperGoldIPStrategy::compute_target_positions(
    MacroTilt tilt, EconRegime regime, double size_mult,
    double china_adj, bool skip_gold_short, double composite) {

    std::unordered_map<std::string, double> pos;
    for (const auto& sym : cg_config_.futures_symbols) {
        pos[sym] = 0.0;
    }

    if (tilt == MacroTilt::NEUTRAL || std::abs(size_mult) < 1e-9) {
        return pos;
    }

    double si_adj = compute_si_vol_adjustment();
    bool strong_signal = !std::isnan(composite) && std::abs(composite) > 0.5;

    if (tilt == MacroTilt::RISK_ON) {
        if (regime == EconRegime::INFLATION_SHOCK) {
            // Long commodities only, no equity beta
            pos["HG"] = contracts_for("HG", 1.0, size_mult) * china_adj;
            pos["CL"] = contracts_for("CL", 1.0, size_mult);
            pos["SI"] = contracts_for("SI", 1.0, size_mult, si_adj);
            // GC, MNQ, ZN, UB = 0
        } else {
            pos["MNQ"] = contracts_for("MNQ", 1.0, size_mult);
            pos["HG"]  = contracts_for("HG",  1.0, size_mult) * china_adj;
            pos["CL"]  = contracts_for("CL",  1.0, size_mult);
            pos["GC"]  = skip_gold_short ? 0.0 : contracts_for("GC", -1.0, size_mult);
            pos["SI"]  = strong_signal ? contracts_for("SI", 1.0, size_mult, si_adj) : 0.0;
            pos["ZN"]  = contracts_for("ZN", -1.0, size_mult);
            pos["UB"]  = contracts_for("UB", -1.0, size_mult);
        }
    } else if (tilt == MacroTilt::RISK_OFF) {
        if (regime == EconRegime::INFLATION_SHOCK) {
            // Long gold, short duration
            pos["GC"] = contracts_for("GC", 1.0, size_mult);
            pos["ZN"] = contracts_for("ZN", -1.0, size_mult);
            pos["UB"] = contracts_for("UB", -1.0, size_mult);
            pos["SI"] = contracts_for("SI", 1.0, size_mult, si_adj);
            // MNQ, HG, CL = 0
        } else {
            pos["MNQ"] = contracts_for("MNQ", -1.0, size_mult);
            pos["HG"]  = contracts_for("HG",  -1.0, size_mult) * china_adj;
            pos["CL"]  = contracts_for("CL",  -1.0, size_mult);
            pos["GC"]  = contracts_for("GC",   1.0, size_mult);
            pos["SI"]  = strong_signal ?
                contracts_for("SI", 1.0, size_mult, si_adj) :
                contracts_for("SI", 1.0, size_mult, si_adj * 0.5);
            pos["ZN"]  = contracts_for("ZN",  1.0, size_mult);
            pos["UB"]  = contracts_for("UB",  1.0, size_mult);
        }
    }

    return pos;
}

// ============================================================
// Position limits
// ============================================================
void CopperGoldIPStrategy::apply_position_limits(
    std::unordered_map<std::string, double>& positions) {

    double equity = signal_state_.equity;
    if (equity <= 0.0) return;

    // Per-instrument notional caps
    for (auto& [sym, qty] : positions) {
        double limit_pct = 0.0;
        std::string ac = asset_class_of(sym);
        if (ac == "equity_index") limit_pct = cg_config_.max_single_equity_notional;
        else if (ac == "commodities") limit_pct = cg_config_.max_single_commodity_notional;
        else continue;

        auto spec_it = CONTRACT_SPECS.find(sym);
        double notional = (spec_it != CONTRACT_SPECS.end()) ? spec_it->second.notional : 100000.0;
        double max_q = std::max(0.0, std::floor(equity * limit_pct / notional));
        if (std::abs(qty) > max_q) {
            qty = std::copysign(max_q, qty);
        }
    }

    // Total equity notional cap
    {
        double eq_not = 0.0;
        for (const auto& [sym, qty] : positions) {
            if (asset_class_of(sym) == "equity_index") {
                auto spec_it = CONTRACT_SPECS.find(sym);
                double notional = (spec_it != CONTRACT_SPECS.end()) ? spec_it->second.notional : 40000.0;
                eq_not += std::abs(qty) * notional;
            }
        }
        double max_eq = equity * cg_config_.max_total_equity_notional;
        if (eq_not > max_eq && eq_not > 0.0) {
            double scale = max_eq / eq_not;
            for (auto& [sym, qty] : positions) {
                if (asset_class_of(sym) == "equity_index") {
                    qty = std::floor(qty * scale + 0.5);
                }
            }
        }
    }

    // Total commodity notional cap
    {
        double com_not = 0.0;
        for (const auto& [sym, qty] : positions) {
            if (asset_class_of(sym) == "commodities") {
                auto spec_it = CONTRACT_SPECS.find(sym);
                double notional = (spec_it != CONTRACT_SPECS.end()) ? spec_it->second.notional : 100000.0;
                com_not += std::abs(qty) * notional;
            }
        }
        double max_com = equity * cg_config_.max_total_commodity_notional;
        if (com_not > max_com && com_not > 0.0) {
            double scale = max_com / com_not;
            for (auto& [sym, qty] : positions) {
                if (asset_class_of(sym) == "commodities") {
                    qty = std::floor(qty * scale + 0.5);
                }
            }
        }
    }
}

// ============================================================
// Rebalance decision
// ============================================================
bool CopperGoldIPStrategy::should_rebalance(
    bool friday, bool tilt_changed, bool regime_changed,
    bool filter_triggered, bool stop_triggered,
    const std::unordered_map<std::string, double>& positions) {

    if (friday || tilt_changed || regime_changed || filter_triggered || stop_triggered) {
        return true;
    }

    // Force rebalance if all positions are zero but we should be trading
    bool all_flat = true;
    for (const auto& [sym, qty] : positions) {
        if (std::abs(qty) > 1e-9) { all_flat = false; break; }
    }
    return all_flat;
}

// ============================================================
// Price history management
// ============================================================
void CopperGoldIPStrategy::update_price_histories(const std::vector<Bar>& bars) {
    for (const auto& bar : bars) {
        auto it = futures_state_.find(bar.symbol);
        if (it == futures_state_.end()) continue;

        auto& state = it->second;
        state.current_price = bar.close.as_double();
        state.price_history.push_back(state.current_price);
        state.last_update = bar.timestamp;
        trim_deque(state.price_history);
    }
}

void CopperGoldIPStrategy::update_macro_state(const DailyMacroRecord& rec) {
    auto push = [this](std::deque<double>& d, double v) {
        d.push_back(v);
        trim_deque(d);
    };

    push(macro_state_.dxy_history, rec.dxy);
    push(macro_state_.vix_history, rec.vix);
    push(macro_state_.hy_spread_history, rec.hy_spread);
    push(macro_state_.breakeven_history, rec.breakeven_10y);
    push(macro_state_.yield_10y_history, rec.yield_10y);
    push(macro_state_.tips_10y_history, rec.tips_10y);
    push(macro_state_.spx_history, rec.spx);
    push(macro_state_.fed_bs_history, rec.fed_balance_sheet);
    push(macro_state_.china_cli_history, rec.china_cli);
}

void CopperGoldIPStrategy::trim_deque(std::deque<double>& d) const {
    while (d.size() > MAX_HISTORY) {
        d.pop_front();
    }
}

// ============================================================
// Rolling statistics
// ============================================================
double CopperGoldIPStrategy::compute_sma(const std::deque<double>& series, int period) const {
    if (series.empty() || period <= 0 || static_cast<int>(series.size()) < period) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double sum = 0.0;
    for (size_t i = series.size() - period; i < series.size(); ++i) {
        sum += series[i];
    }
    return sum / period;
}

double CopperGoldIPStrategy::compute_std(const std::deque<double>& series, int period) const {
    if (series.empty() || period <= 0 || static_cast<int>(series.size()) < period) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double mean = compute_sma(series, period);
    if (std::isnan(mean)) return std::numeric_limits<double>::quiet_NaN();

    double var = 0.0;
    for (size_t i = series.size() - period; i < series.size(); ++i) {
        double diff = series[i] - mean;
        var += diff * diff;
    }
    return std::sqrt(var / period);
}

double CopperGoldIPStrategy::compute_zscore_val(const std::deque<double>& series, int window) const {
    double mean = compute_sma(series, window);
    double std_val = compute_std(series, window);
    if (std::isnan(mean) || std::isnan(std_val) || std_val <= 0.0 || series.empty()) return 0.0;
    return (series.back() - mean) / std_val;
}

double CopperGoldIPStrategy::compute_roc(const std::deque<double>& series, int lookback) const {
    if (static_cast<int>(series.size()) <= lookback) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double past = series[series.size() - lookback - 1];
    if (past <= 0.0) return std::numeric_limits<double>::quiet_NaN();
    return (series.back() / past) - 1.0;
}

double CopperGoldIPStrategy::compute_percentile(
    const std::deque<double>& series, int window, double current) const {

    if (static_cast<int>(series.size()) < window) return 0.5;

    std::vector<double> window_data;
    for (size_t i = series.size() - window; i < series.size(); ++i) {
        window_data.push_back(series[i]);
    }
    std::sort(window_data.begin(), window_data.end());

    auto it = std::lower_bound(window_data.begin(), window_data.end(), current);
    int rank = std::distance(window_data.begin(), it);
    return static_cast<double>(rank) / window_data.size();
}

double CopperGoldIPStrategy::compute_momentum(const std::deque<double>& series, int lookback) const {
    if (static_cast<int>(series.size()) <= lookback) return 0.0;
    double past = series[series.size() - lookback - 1];
    if (past <= 0.0) return 0.0;
    return (series.back() / past) - 1.0;
}

// ============================================================
// Utility
// ============================================================
std::unordered_map<std::string, std::vector<double>> CopperGoldIPStrategy::get_price_history() const {
    std::unordered_map<std::string, std::vector<double>> history;
    for (const auto& [sym, state] : futures_state_) {
        history[sym] = std::vector<double>(state.price_history.begin(),
                                            state.price_history.end());
    }
    return history;
}

CopperGoldIPStrategy::DateParts CopperGoldIPStrategy::extract_date(const Timestamp& ts) {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm* tm = std::localtime(&time_t);
    return {tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday};
}

bool CopperGoldIPStrategy::is_friday(const Timestamp& ts) {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm* tm = std::localtime(&time_t);
    return tm->tm_wday == 5;
}

std::string CopperGoldIPStrategy::asset_class_of(const std::string& sym) {
    if (sym == "MNQ" || sym == "MES") return "equity_index";
    if (sym == "CL" || sym == "HG" || sym == "GC" || sym == "SI") return "commodities";
    if (sym == "ZN" || sym == "UB") return "fixed_income";
    return "commodities";
}

}  // namespace trade_ngin
