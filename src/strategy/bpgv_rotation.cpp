// src/strategy/bpgv_rotation.cpp
#include "trade_ngin/strategy/bpgv_rotation.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/csv_equity_loader.hpp"
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {

// Per-asset extreme allocation tables
// Weights are looked up by symbol from the config's risk_on_symbols / risk_off_symbols.
// If a symbol isn't found in the table, it gets an equal share within its bucket.
// These defaults match the Python V3 ETF weights; the backtest entry point can
// override them via config when using stock proxies instead of ETFs.
const std::unordered_map<std::string, double> BPGVRotationStrategy::RISK_ON_EXTREME = {};
const std::unordered_map<std::string, double> BPGVRotationStrategy::RISK_OFF_EXTREME = {};

// ============================================================================
// Tier-1 nested config from_json implementations
// ============================================================================

void CrashTriggerConfig::from_json(const nlohmann::json& j) {
    if (j.contains("method")) method = j.at("method").get<std::string>();
    if (j.contains("lookback_days")) lookback_days = j.at("lookback_days").get<int>();
    if (j.contains("k_sigma")) k_sigma = j.at("k_sigma").get<double>();
    if (j.contains("vol_gate_percentile"))
        vol_gate_percentile = j.at("vol_gate_percentile").get<double>();
    if (j.contains("vol_gate_window")) vol_gate_window = j.at("vol_gate_window").get<int>();
    if (j.contains("min_history_days"))
        min_history_days = j.at("min_history_days").get<int>();
}

void CrashExitConfig::from_json(const nlohmann::json& j) {
    if (j.contains("method")) method = j.at("method").get<std::string>();
    if (j.contains("min_hold_days")) min_hold_days = j.at("min_hold_days").get<int>();
    if (j.contains("confirmation_days"))
        confirmation_days = j.at("confirmation_days").get<int>();
    if (j.contains("exit_threshold"))
        exit_threshold = j.at("exit_threshold").get<double>();
    if (j.contains("max_hold_days"))
        max_hold_days = j.at("max_hold_days").is_null() ? -1
                                                        : j.at("max_hold_days").get<int>();
    if (j.contains("trend_symbol"))
        trend_symbol = j.at("trend_symbol").get<std::string>();
    if (j.contains("weights")) {
        const auto& w = j.at("weights");
        if (w.contains("trend")) weight_trend = w.at("trend").get<double>();
        if (w.contains("vol_norm")) weight_vol_norm = w.at("vol_norm").get<double>();
        if (w.contains("recov")) weight_recov = w.at("recov").get<double>();
    }
}

void CrashOverrideConfig::from_json(const nlohmann::json& j) {
    if (j.contains("defensive_weights")) {
        defensive_weights.clear();
        for (const auto& [k, v] : j.at("defensive_weights").items()) {
            defensive_weights[k] = v.get<double>();
        }
    }
    if (j.contains("zero_symbols")) {
        zero_symbols = j.at("zero_symbols").get<std::vector<std::string>>();
    }
    if (j.contains("splice_fallback_symbol")) {
        splice_fallback_symbol = j.at("splice_fallback_symbol").get<std::string>();
    }
    if (j.contains("trigger")) {
        trigger.from_json(j.at("trigger"));
    }
    if (j.contains("exit")) {
        exit.from_json(j.at("exit"));
    }
}

void RebalanceConfig::from_json(const nlohmann::json& j) {
    if (j.contains("drift_abs_trigger"))
        drift_abs_trigger = j.at("drift_abs_trigger").get<double>();
    if (j.contains("drift_rel_trigger"))
        drift_rel_trigger = j.at("drift_rel_trigger").get<double>();
    if (j.contains("halfway_rule"))
        halfway_rule = j.at("halfway_rule").get<bool>();
    if (j.contains("exit_fully_on_zero_target"))
        exit_fully_on_zero_target = j.at("exit_fully_on_zero_target").get<bool>();
    if (j.contains("enter_fully_on_zero_current"))
        enter_fully_on_zero_current = j.at("enter_fully_on_zero_current").get<bool>();
}

void MomentumConfig::from_json(const nlohmann::json& j) {
    if (j.contains("tsm_gate_enabled")) tsm_gate_enabled = j.at("tsm_gate_enabled").get<bool>();
    if (j.contains("tsm_lookback_days")) tsm_lookback_days = j.at("tsm_lookback_days").get<int>();
    if (j.contains("tsm_risk_free_symbol"))
        tsm_risk_free_symbol = j.at("tsm_risk_free_symbol").get<std::string>();
    if (j.contains("tsm_fail_open_on_short_history"))
        tsm_fail_open_on_short_history = j.at("tsm_fail_open_on_short_history").get<bool>();
    if (j.contains("tsm_tolerance"))
        tsm_tolerance = j.at("tsm_tolerance").get<double>();
    if (j.contains("xsec_lookback_days"))
        xsec_lookback_days = j.at("xsec_lookback_days").get<int>();
    if (j.contains("xsec_use_vol_scaling"))
        xsec_use_vol_scaling = j.at("xsec_use_vol_scaling").get<bool>();
    if (j.contains("xsec_vol_window_days"))
        xsec_vol_window_days = j.at("xsec_vol_window_days").get<int>();
    if (j.contains("xsec_sigma_floor"))
        xsec_sigma_floor = j.at("xsec_sigma_floor").get<double>();
    if (j.contains("xsec_tau")) xsec_tau = j.at("xsec_tau").get<double>();
    if (j.contains("xsec_weight_floor"))
        xsec_weight_floor = j.at("xsec_weight_floor").get<double>();
}

void BreakoutConfig::from_json(const nlohmann::json& j) {
    if (j.contains("mode")) mode = j.at("mode").get<std::string>();
    if (j.contains("sma_window")) sma_window = j.at("sma_window").get<int>();
    if (j.contains("atr_window")) atr_window = j.at("atr_window").get<int>();
    if (j.contains("atr_k")) atr_k = j.at("atr_k").get<double>();
    if (j.contains("confirmation_days"))
        confirmation_days = j.at("confirmation_days").get<int>();
    if (j.contains("graded_weighting"))
        graded_weighting = j.at("graded_weighting").get<bool>();
    if (j.contains("index_gate_enabled"))
        index_gate_enabled = j.at("index_gate_enabled").get<bool>();
    if (j.contains("index_gate_symbol"))
        index_gate_symbol = j.at("index_gate_symbol").get<std::string>();
    if (j.contains("index_gate_floor"))
        index_gate_floor = j.at("index_gate_floor").get<double>();
    if (j.contains("fail_open_on_short_history"))
        fail_open_on_short_history = j.at("fail_open_on_short_history").get<bool>();
}

BPGVRotationStrategy::BPGVRotationStrategy(
    std::string id, StrategyConfig config, BPGVRotationConfig bpgv_config,
    std::shared_ptr<PostgresDatabase> db, std::shared_ptr<InstrumentRegistry> registry)
    : BaseStrategy(std::move(id), std::move(config), std::move(db)),
      bpgv_config_(std::move(bpgv_config)),
      registry_(registry) {
    Logger::register_component("BPGVRotation");

    metadata_.name = "BPGV Macro Regime Rotation Strategy";
    metadata_.description = "Macro-driven regime rotation using building permit growth volatility";
}

Result<void> BPGVRotationStrategy::validate_config() const {
    auto result = BaseStrategy::validate_config();
    if (result.is_error()) return result;

    if (bpgv_config_.rebalance_day_of_month < 1 || bpgv_config_.rebalance_day_of_month > 28) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Rebalance day must be between 1 and 28",
                                "BPGVRotationStrategy");
    }

    if (bpgv_config_.crash_threshold >= 0.0) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Crash threshold must be negative",
                                "BPGVRotationStrategy");
    }

    if (bpgv_config_.risk_on_symbols.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Risk-on symbols list cannot be empty",
                                "BPGVRotationStrategy");
    }

    if (bpgv_config_.risk_off_symbols.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "Risk-off symbols list cannot be empty",
                                "BPGVRotationStrategy");
    }

    // Tier-1: defensive basket is required. Guardrail #4 (no silent fallback
    // to a hardcoded crash basket). Must sum to 1.0 ± 1e-6.
    if (bpgv_config_.crash_override.defensive_weights.empty()) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "crash_override.defensive_weights is required (Tier-1). "
                                "Add a 'crash_override.defensive_weights' block to the "
                                "strategy config.",
                                "BPGVRotationStrategy");
    }
    double sum = 0.0;
    for (const auto& [_, w] : bpgv_config_.crash_override.defensive_weights) {
        sum += w;
    }
    if (std::abs(sum - 1.0) > 1e-6) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "crash_override.defensive_weights must sum to 1.0 (got " + std::to_string(sum) + ")",
            "BPGVRotationStrategy");
    }

    return Result<void>();
}

Result<void> BPGVRotationStrategy::initialize() {
    auto base_result = BaseStrategy::initialize();
    if (base_result.is_error()) {
        ERROR("Base strategy initialization failed: " + std::string(base_result.error()->what()));
        return base_result;
    }

    set_pnl_accounting_method(PnLAccountingMethod::MIXED);
    INFO("BPGV rotation strategy using MIXED PnL accounting");

    // Load macro regime data from CSV
    auto macro_result = MacroCSVLoader::load(bpgv_config_.macro_csv_path);
    if (macro_result.is_error()) {
        ERROR("Failed to load macro CSV: " + std::string(macro_result.error()->what()));
        return make_error<void>(ErrorCode::NOT_INITIALIZED,
                                "Cannot initialize BPGV strategy without macro data: " +
                                    std::string(macro_result.error()->what()),
                                "BPGVRotationStrategy");
    }
    macro_data_ = macro_result.value();
    INFO("Loaded " + std::to_string(macro_data_.size()) + " macro regime records (" +
         std::to_string(macro_data_.front().year) + "-" +
         std::to_string(macro_data_.back().year) + ")");

    // Initialize per-symbol state and positions for every asset — risk-on,
    // risk-off, AND cash symbols. Cash symbols (BIL, DBMF) need state because
    // (a) their price history feeds the TSM risk-free lookup, (b) they
    // appear in the crash-override defensive basket. They do NOT get base
    // weight during normal regimes (see calculate_base_weights below).
    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.cash_symbols.begin(),
                       bpgv_config_.cash_symbols.end());

    for (const auto& symbol : all_symbols) {
        symbol_state_[symbol] = SymbolState{};

        Position pos;
        pos.symbol = symbol;
        pos.quantity = 0.0;
        pos.average_price = 0.0;
        pos.last_update = std::chrono::system_clock::now();
        positions_[symbol] = pos;
    }

    portfolio_value_ = config_.capital_allocation;

    // Tier-1 remediation (Fix 3): pre-load ~520 days of OHLC into each symbol's
    // history so long-lookback filters (252 d TSM, 200 d SMA, 504 d vol gate)
    // fire on day 1 of the live backtest window instead of waiting ~10 months.
    //
    // Look-ahead safety: we load [warmup_start_date - warmup_days,
    //   warmup_start_date - 1 day] inclusive. The backtest coordinator starts
    // feeding bars at warmup_start_date, so the pre-load strictly precedes any
    // live bar. Guardrail #6.
    if (bpgv_config_.warmup_start_date.time_since_epoch().count() != 0 &&
        bpgv_config_.warmup_days > 0) {
        auto warmup_end   = bpgv_config_.warmup_start_date - std::chrono::hours(24);
        auto warmup_start = bpgv_config_.warmup_start_date -
                            std::chrono::hours(24 * bpgv_config_.warmup_days);

        int total_preloaded = 0;
        for (const auto& symbol : all_symbols) {
            std::vector<Bar> preload_bars;

            // CSV first, DB fallback. During warmup we want the data source
            // with the deepest history. CSVs under data/equity_bars/ go back
            // to 1999+ for most symbols; the DB (macro_data.bsts_etf_prices)
            // only starts 2011-03-23 for SPY/TLT/GLD — not enough for a 520-day
            // warmup window that reaches into 2009. EQR is an exception
            // (DB-only via equities_data.ohlcv_1d) and correctly falls through.
            auto csv_result = CSVEquityLoader::load(
                symbol, warmup_start, warmup_end, "data/equity_bars");
            if (csv_result.is_ok()) {
                preload_bars = csv_result.value();
            }
            if (preload_bars.empty() && db_ && db_->is_connected()) {
                auto db_result = db_->get_market_data(
                    {symbol}, warmup_start, warmup_end,
                    AssetClass::EQUITIES, DataFrequency::DAILY, "ohlcv");
                if (db_result.is_ok() && db_result.value()) {
                    auto bars_result = DataConversionUtils::arrow_table_to_bars(
                        db_result.value());
                    if (bars_result.is_ok()) {
                        preload_bars = bars_result.value();
                    }
                }
            }

            if (preload_bars.empty()) {
                WARN("Warmup pre-load: no history available for " + symbol +
                     " — filters will fall back to their fail-open paths");
                continue;
            }

            // Push each bar into the symbol's rolling histories. Shares the
            // exact push semantics used by update_price_histories() in the live
            // path so ATR / SMA / TSM see identical data.
            auto& state = symbol_state_[symbol];
            for (const auto& bar : preload_bars) {
                state.current_price = bar.close.as_double();
                state.price_history.push_back(state.current_price);
                state.high_history.push_back(bar.high.as_double());
                state.low_history.push_back(bar.low.as_double());
                state.last_update = bar.timestamp;
                trim_price_history(state);
            }
            total_preloaded += static_cast<int>(preload_bars.size());
            DEBUG("Warmup pre-load: " + symbol + " " +
                  std::to_string(preload_bars.size()) + " bars");
        }
        INFO("BPGV warmup pre-load complete: " +
             std::to_string(total_preloaded) + " bars across " +
             std::to_string(all_symbols.size()) + " symbols");
    }

    INFO("BPGV rotation strategy initialized for " + std::to_string(all_symbols.size()) + " symbols");
    return Result<void>();
}

// ============================================================================
// on_data — daily heartbeat
// ============================================================================

Result<void> BPGVRotationStrategy::on_data(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "Strategy is not in running state",
                                "BPGVRotationStrategy");
    }

    if (data.empty()) return Result<void>();

    try {
        // 1. Update price histories
        update_price_histories(data);

        // 2. Compute and track portfolio value for crash detection. Keep enough
        // history for whichever trigger method is active — vol-scaled needs the
        // full vol_gate_window (default 504) for percentile ranking; fixed needs
        // only crash_lookback_days + slack. Also retain 60 days for exit-score
        // `recov` component (Change 4).
        double pv = compute_portfolio_value();
        if (pv > 0.0) {
            portfolio_value_ = pv;
        }
        portfolio_value_history_.push_back(portfolio_value_);
        {
            size_t needed = static_cast<size_t>(bpgv_config_.crash_lookback_days + 10);
            if (bpgv_config_.crash_override.trigger.method == "volatility_scaled") {
                needed = std::max(needed, static_cast<size_t>(
                                               bpgv_config_.crash_override.trigger.vol_gate_window + 10));
            }
            needed = std::max(needed, static_cast<size_t>(70));  // Change 4: recov window + slack
            while (portfolio_value_history_.size() > needed) {
                portfolio_value_history_.pop_front();
            }
        }

        // 3. Extract date from bars
        auto date = extract_date(data.front().timestamp);

        // 4. Crash override management
        if (crash_override_active_) {
            const auto& ecfg = bpgv_config_.crash_override.exit;

            // Change 4: advance signal-contingent confirmation counter before
            // we ask whether to exit. The counter tracks consecutive days on
            // which the composite score is above threshold; a single bad day
            // resets it.
            if (ecfg.method == "signal_contingent") {
                auto trend_it = symbol_state_.find(ecfg.trend_symbol);
                if (trend_it != symbol_state_.end()) {
                    double score = compute_exit_score(trend_it->second.price_history);
                    if (score > ecfg.exit_threshold) {
                        ++consecutive_good_exit_days_;
                    } else {
                        consecutive_good_exit_days_ = 0;
                    }
                }
            }

            if (is_crash_override_expired(data.front().timestamp)) {
                crash_override_active_ = false;
                consecutive_good_exit_days_ = 0;
                INFO("Crash override expired, resuming normal regime");
                // Force rebalance on next eligible day by clearing last rebalance tracking
                last_rebalance_year_ = 0;
                last_rebalance_month_ = 0;
            }
            // While crash override is active, keep crash weights — don't rebalance
        }

        if (!crash_override_active_ && detect_crash()) {
            activate_crash_override(data.front().timestamp);
            current_weights_ = build_crash_weights();
            update_positions_from_weights();
        }

        // 5. Monthly rebalance check (only if no crash override)
        if (!crash_override_active_ && should_rebalance(date.year, date.month, date.day)) {
            execute_rebalance(date.year, date.month);
        }

        // 6. Update position timestamps (DO NOT set average_price -- on_execution manages cost basis)
        for (const auto& bar : data) {
            auto pos_it = positions_.find(bar.symbol);
            if (pos_it != positions_.end()) {
                pos_it->second.last_update = bar.timestamp;
            }
        }

        return Result<void>();

    } catch (const std::exception& e) {
        ERROR("Error in BPGVRotationStrategy::on_data: " + std::string(e.what()));
        return make_error<void>(ErrorCode::UNKNOWN_ERROR,
                                "Failed to process market data: " + std::string(e.what()),
                                "BPGVRotationStrategy");
    }
}

// ============================================================================
// Price history management
// ============================================================================

void BPGVRotationStrategy::update_price_histories(const std::vector<Bar>& bars) {
    for (const auto& bar : bars) {
        auto it = symbol_state_.find(bar.symbol);
        if (it == symbol_state_.end()) continue;

        auto& state = it->second;
        state.current_price = bar.close.as_double();
        state.price_history.push_back(state.current_price);
        state.high_history.push_back(bar.high.as_double());
        state.low_history.push_back(bar.low.as_double());
        state.last_update = bar.timestamp;
        trim_price_history(state);
    }
}

void BPGVRotationStrategy::trim_price_history(SymbolState& state) const {
    while (state.price_history.size() > MAX_PRICE_HISTORY) {
        state.price_history.pop_front();
    }
    while (state.high_history.size() > MAX_PRICE_HISTORY) {
        state.high_history.pop_front();
    }
    while (state.low_history.size() > MAX_PRICE_HISTORY) {
        state.low_history.pop_front();
    }
}

// ============================================================================
// Rebalancing
// ============================================================================

bool BPGVRotationStrategy::should_rebalance(int year, int month, int day) const {
    if (day < bpgv_config_.rebalance_day_of_month) return false;
    if (year == last_rebalance_year_ && month == last_rebalance_month_) return false;
    return true;
}

void BPGVRotationStrategy::execute_rebalance(int year, int month) {
    // Use PREVIOUS month's regime data (permits released mid-month)
    int prev_year = (month == 1) ? year - 1 : year;
    int prev_month = (month == 1) ? 12 : month - 1;

    auto record = MacroCSVLoader::find_record(macro_data_, prev_year, prev_month);
    if (!record.has_value()) {
        // Fallback to most recent available
        record = MacroCSVLoader::find_record_before(macro_data_, prev_year, prev_month);
        if (!record.has_value()) {
            WARN("No macro data available for rebalance at " +
                 std::to_string(year) + "-" + std::to_string(month) + ", skipping");
            return;
        }
        WARN("Using fallback macro data from " +
             std::to_string(record->year) + "-" + std::to_string(record->month));
    }

    // Weight calculation pipeline
    auto weights = calculate_base_weights(*record);
    apply_momentum_tilt(weights);
    apply_homebuilder_tilt(weights, *record);
    apply_breakout_filter(weights);
    normalize_weights(weights);

    current_weights_ = weights;
    update_positions_from_weights();

    last_rebalance_year_ = year;
    last_rebalance_month_ = month;

    DEBUG("Rebalanced " + std::to_string(year) + "-" + std::to_string(month) +
          " | regime_score=" + std::to_string(record->regime_score) +
          " | strong_risk_on=" + std::to_string(record->strong_risk_on));
}

// ============================================================================
// Weight calculation pipeline
// ============================================================================

std::unordered_map<std::string, double> BPGVRotationStrategy::calculate_base_weights(
    const MonthlyMacroRecord& rec) const {

    // Interpolation: risk_off_pct maps [-1, +1] to [5%, 45%]
    double risk_off_pct = bpgv_config_.base_risk_off_min +
                          bpgv_config_.base_risk_off_range * ((rec.regime_score + 1.0) / 2.0);
    double risk_on_pct = 1.0 - risk_off_pct;

    std::unordered_map<std::string, double> weights;

    // Use configured extreme weights, or fall back to equal weight within bucket
    const auto& on_extreme = bpgv_config_.risk_on_extreme_weights;
    const auto& off_extreme = bpgv_config_.risk_off_extreme_weights;

    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());

    double n_risk_on = static_cast<double>(bpgv_config_.risk_on_symbols.size());
    double n_risk_off = static_cast<double>(bpgv_config_.risk_off_symbols.size());

    for (const auto& sym : all_symbols) {
        double on_w = 0.0;
        double off_w = 0.0;

        if (!on_extreme.empty()) {
            auto it = on_extreme.find(sym);
            if (it != on_extreme.end()) on_w = it->second;
        } else {
            // Equal weight fallback: risk-on assets get 1/N of 95%, risk-off get 1/N of 5%
            bool is_risk_on = std::find(bpgv_config_.risk_on_symbols.begin(),
                                         bpgv_config_.risk_on_symbols.end(), sym)
                              != bpgv_config_.risk_on_symbols.end();
            on_w = is_risk_on ? (0.95 / n_risk_on) : (0.05 / n_risk_off);
        }

        if (!off_extreme.empty()) {
            auto it = off_extreme.find(sym);
            if (it != off_extreme.end()) off_w = it->second;
        } else {
            bool is_risk_on = std::find(bpgv_config_.risk_on_symbols.begin(),
                                         bpgv_config_.risk_on_symbols.end(), sym)
                              != bpgv_config_.risk_on_symbols.end();
            off_w = is_risk_on ? (0.30 / n_risk_on) : (0.70 / n_risk_off);
        }

        weights[sym] = on_w * risk_on_pct + off_w * risk_off_pct;
    }

    // Strong risk-on boost: tilt toward equities, away from bonds
    if (rec.strong_risk_on) {
        for (const auto& sym : bpgv_config_.risk_on_symbols) {
            weights[sym] *= (1.0 + bpgv_config_.strong_risk_on_equity_boost);
        }
        for (const auto& sym : bpgv_config_.risk_off_symbols) {
            weights[sym] *= (1.0 - bpgv_config_.strong_risk_on_bond_reduction);
        }
    }

    return weights;
}

double BPGVRotationStrategy::compute_symbol_vol(const std::deque<double>& prices, int window) const {
    int n = static_cast<int>(prices.size());
    if (n < window + 1) return 0.0;
    std::vector<double> rets;
    rets.reserve(window);
    for (int i = n - window; i < n; ++i) {
        double prev = prices[i - 1];
        double cur = prices[i];
        if (prev < 1e-8) { rets.push_back(0.0); continue; }
        rets.push_back((cur - prev) / prev);
    }
    if (rets.size() < 2) return 0.0;
    double mean = 0.0;
    for (double r : rets) mean += r;
    mean /= rets.size();
    double var = 0.0;
    for (double r : rets) var += (r - mean) * (r - mean);
    var /= (rets.size() - 1);
    // Annualize (sqrt(252) trading days).
    return std::sqrt(var) * std::sqrt(252.0);
}

void BPGVRotationStrategy::apply_momentum_tilt(
    std::unordered_map<std::string, double>& weights) const {

    // Change 5: two-layer momentum pipeline.
    //   1. Absolute TSM gate (Moskowitz-Ooi-Pedersen 2012, Antonacci 2014):
    //      zero any risk-on name whose 12-month excess return over BIL is
    //      negative. Fail-open if history is shorter than the lookback.
    //   2. Vol-scaled cross-sectional ranker (Barroso-Santa-Clara 2015):
    //      score = r_126d / sigma_126d_annualized. Rank descending, apply
    //      symmetric tilt ±tau clipped at weight_floor.
    const auto& mcfg = bpgv_config_.momentum;

    // ---- Step 1: TSM absolute gate ----------------------------------------
    if (mcfg.tsm_gate_enabled) {
        // Look up BIL 12m return once; if not yet available we use rf = 0 and
        // (in strict mode) log a warning so the run still produces a signal.
        double rf_excess = 0.0;
        auto rf_it = symbol_state_.find(mcfg.tsm_risk_free_symbol);
        if (rf_it != symbol_state_.end() &&
            static_cast<int>(rf_it->second.price_history.size()) > mcfg.tsm_lookback_days) {
            rf_excess = calculate_trailing_return(rf_it->second.price_history,
                                                  mcfg.tsm_lookback_days);
        } else if (!mcfg.tsm_fail_open_on_short_history) {
            WARN("TSM gate: risk-free symbol " + mcfg.tsm_risk_free_symbol +
                 " has insufficient history, treating rf_12m = 0");
        }

        for (const auto& sym : bpgv_config_.risk_on_symbols) {
            auto it = symbol_state_.find(sym);
            if (it == symbol_state_.end()) continue;

            if (static_cast<int>(it->second.price_history.size()) <= mcfg.tsm_lookback_days) {
                if (!mcfg.tsm_fail_open_on_short_history) {
                    weights[sym] = 0.0;  // strict mode: cut when history is short
                }
                continue;  // fail-open: skip the gate for this name
            }

            double r_12m = calculate_trailing_return(it->second.price_history,
                                                    mcfg.tsm_lookback_days);
            double excess = r_12m - rf_excess;
            // Tier-1 remediation: allow a tolerance zone so marginally-negative
            // names still pass the gate. `excess <= -tolerance` means only cut
            // names whose 12 m excess return is worse than -`tolerance`.
            if (excess <= -mcfg.tsm_tolerance) {
                weights[sym] = 0.0;
            }
        }
    }

    // ---- Step 2: Vol-scaled cross-sectional ranker ------------------------
    struct MomentumEntry {
        std::string symbol;
        double score;
    };
    std::vector<MomentumEntry> entries;

    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        // Skip names already zeroed by TSM gate — ranking them would be wasted.
        auto wit = weights.find(sym);
        if (wit == weights.end() || wit->second <= 1e-8) continue;

        auto it = symbol_state_.find(sym);
        if (it == symbol_state_.end()) continue;

        if (static_cast<int>(it->second.price_history.size()) <= mcfg.xsec_lookback_days) {
            continue;  // not enough history to compute the xsec score
        }

        double r_xsec = calculate_trailing_return(it->second.price_history,
                                                  mcfg.xsec_lookback_days);
        double score = r_xsec;
        if (mcfg.xsec_use_vol_scaling) {
            double sigma = compute_symbol_vol(it->second.price_history, mcfg.xsec_vol_window_days);
            double sigma_eff = std::max(sigma, mcfg.xsec_sigma_floor);
            score = r_xsec / sigma_eff;
        }
        entries.push_back({sym, score});
    }

    if (entries.size() < 2) return;

    std::sort(entries.begin(), entries.end(),
              [](const MomentumEntry& a, const MomentumEntry& b) { return a.score > b.score; });

    int n = static_cast<int>(entries.size());
    for (int i = 0; i < n; ++i) {
        double rank_score = (n > 1) ? (1.0 - 2.0 * i / (n - 1)) : 0.0;  // [+1, -1]
        double tilt = 1.0 + mcfg.xsec_tau * rank_score;
        tilt = std::max(mcfg.xsec_weight_floor, tilt);
        weights[entries[i].symbol] *= tilt;
    }
}

void BPGVRotationStrategy::apply_homebuilder_tilt(
    std::unordered_map<std::string, double>& weights,
    const MonthlyMacroRecord& rec) const {

    auto it = weights.find(bpgv_config_.homebuilder_symbol);
    if (it == weights.end()) return;

    if (rec.permit_growth > 0.0) {
        it->second *= (1.0 + bpgv_config_.homebuilder_tilt_scale);
    } else {
        it->second *= std::max(0.2, 1.0 - bpgv_config_.homebuilder_tilt_scale);
    }
}

// --- Change 6: Wilder ATR + graded breakout ---------------------------------

double BPGVRotationStrategy::wilder_atr(const std::deque<double>& high,
                                        const std::deque<double>& low,
                                        const std::deque<double>& close,
                                        int period) {
    int n = static_cast<int>(close.size());
    if (n <= period || static_cast<int>(high.size()) != n ||
        static_cast<int>(low.size()) != n) {
        return 0.0;
    }

    // True range series TR_i = max(H_i - L_i, |H_i - C_{i-1}|, |L_i - C_{i-1}|).
    std::vector<double> tr;
    tr.reserve(n - 1);
    for (int i = 1; i < n; ++i) {
        double h_l = high[i] - low[i];
        double h_pc = std::abs(high[i] - close[i - 1]);
        double l_pc = std::abs(low[i] - close[i - 1]);
        tr.push_back(std::max({h_l, h_pc, l_pc}));
    }

    // Wilder seed: simple average of first `period` TR values.
    if (static_cast<int>(tr.size()) < period) return 0.0;
    double atr = 0.0;
    for (int i = 0; i < period; ++i) atr += tr[i];
    atr /= period;

    // Wilder recursive smoothing: ATR_i = ((period - 1) * ATR_{i-1} + TR_i) / period.
    for (size_t i = period; i < tr.size(); ++i) {
        atr = ((period - 1) * atr + tr[i]) / period;
    }
    return atr;
}

double BPGVRotationStrategy::breakout_score(const std::deque<double>& high,
                                            const std::deque<double>& low,
                                            const std::deque<double>& close) const {
    const auto& bcfg = bpgv_config_.breakout;

    int n = static_cast<int>(close.size());
    int needed = std::max(bcfg.sma_window + 1, bcfg.atr_window + 1);
    if (n < needed) {
        return bcfg.fail_open_on_short_history ? 1.0 : 0.0;
    }

    double sma = calculate_sma(close, bcfg.sma_window);
    double atr = wilder_atr(high, low, close, bcfg.atr_window);

    // Confirmation: require `confirmation_days` consecutive closes below SMA
    // to zero the name. Faber (2007/2013) — cuts whipsaws dramatically vs. a
    // single-day cut rule.
    if (bcfg.confirmation_days > 0 && static_cast<int>(close.size()) >= bcfg.confirmation_days) {
        int count_below = 0;
        for (int i = n - bcfg.confirmation_days; i < n; ++i) {
            if (close[i] < sma) ++count_below;
        }
        if (count_below == bcfg.confirmation_days) return 0.0;
    }

    if (!bcfg.graded_weighting) {
        // Binary mode: 1.0 when close >= SMA, 0.0 otherwise.
        return (close.back() >= sma) ? 1.0 : 0.0;
    }

    // Graded: (close - SMA) / (k * ATR), clipped to [0, 1].
    if (atr < 1e-10) {
        return (close.back() >= sma) ? 1.0 : 0.0;
    }
    double raw = (close.back() - sma) / (bcfg.atr_k * atr);
    return std::max(0.0, std::min(1.0, raw));
}

void BPGVRotationStrategy::apply_breakout_filter(
    std::unordered_map<std::string, double>& weights) const {

    const auto& bcfg = bpgv_config_.breakout;

    // Legacy binary mode: reproduces the baseline 50-day hard cut for A/B.
    if (bcfg.mode == "binary_legacy") {
        int window = bpgv_config_.breakout_sma_window;
        for (const auto& sym : bpgv_config_.risk_on_symbols) {
            auto state_it = symbol_state_.find(sym);
            if (state_it == symbol_state_.end()) continue;
            const auto& ph = state_it->second.price_history;
            if (static_cast<int>(ph.size()) < window) continue;
            double sma = calculate_sma(ph, window);
            double current_price = state_it->second.current_price;
            if (current_price < sma) weights[sym] = 0.0;
        }
        return;
    }

    // Graded mode (Change 6 default).
    //
    // Per-symbol graded multiplier based on distance above 200d SMA scaled by
    // 14-period Wilder ATR. Applied multiplicatively so a price exactly at
    // SMA zeroes out, and > 2×ATR above SMA keeps full weight.
    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto state_it = symbol_state_.find(sym);
        if (state_it == symbol_state_.end()) continue;
        double m = breakout_score(state_it->second.high_history,
                                  state_it->second.low_history,
                                  state_it->second.price_history);
        weights[sym] *= m;
    }

    // Index gate (Asness-Moskowitz-Pedersen 2013): broad-market override.
    // When the index itself is below its own trend, scale the entire risk-on
    // book by the index's graded score (floored at `index_gate_floor`).
    if (bcfg.index_gate_enabled) {
        auto idx_it = symbol_state_.find(bcfg.index_gate_symbol);
        if (idx_it != symbol_state_.end()) {
            double idx_score = breakout_score(idx_it->second.high_history,
                                              idx_it->second.low_history,
                                              idx_it->second.price_history);
            double index_scale = std::max(bcfg.index_gate_floor, idx_score);
            for (const auto& sym : bpgv_config_.risk_on_symbols) {
                weights[sym] *= index_scale;
            }
        }
    }
}

void BPGVRotationStrategy::normalize_weights(
    std::unordered_map<std::string, double>& weights) const {

    double total = 0.0;
    for (const auto& [sym, w] : weights) {
        total += std::max(0.0, w);
    }

    if (total < 1e-8) {
        // Degenerate case (all filters zeroed everything). Equal weight across
        // the entire universe including cash symbols, so the book is still
        // 100 % invested instead of empty.
        auto all_symbols = bpgv_config_.risk_on_symbols;
        all_symbols.insert(all_symbols.end(),
                           bpgv_config_.risk_off_symbols.begin(),
                           bpgv_config_.risk_off_symbols.end());
        all_symbols.insert(all_symbols.end(),
                           bpgv_config_.cash_symbols.begin(),
                           bpgv_config_.cash_symbols.end());
        double eq = 1.0 / static_cast<double>(all_symbols.size());
        for (const auto& sym : all_symbols) {
            weights[sym] = eq;
        }
        return;
    }

    for (auto& [sym, w] : weights) {
        w = std::max(0.0, w) / total;
    }
}

// ============================================================================
// Crash detection and override
// ============================================================================

// --- Change 3 helpers -------------------------------------------------------

double BPGVRotationStrategy::compute_daily_return_stdev(int window) const {
    // Stdev of daily log-returns of portfolio_value_history_ over the last `window` days.
    int n = static_cast<int>(portfolio_value_history_.size());
    if (n < window + 1) return 0.0;
    std::vector<double> rets;
    rets.reserve(window);
    for (int i = n - window; i < n; ++i) {
        double prev = portfolio_value_history_[i - 1];
        double cur = portfolio_value_history_[i];
        if (prev < 1e-8) { rets.push_back(0.0); continue; }
        rets.push_back((cur - prev) / prev);
    }
    double mean = 0.0;
    for (double r : rets) mean += r;
    mean /= static_cast<double>(rets.size());
    double var = 0.0;
    for (double r : rets) var += (r - mean) * (r - mean);
    var /= static_cast<double>(rets.size() - 1);
    return std::sqrt(var);
}

double BPGVRotationStrategy::compute_daily_return_mean(int window) const {
    int n = static_cast<int>(portfolio_value_history_.size());
    if (n < window + 1) return 0.0;
    double sum = 0.0;
    int count = 0;
    for (int i = n - window; i < n; ++i) {
        double prev = portfolio_value_history_[i - 1];
        double cur = portfolio_value_history_[i];
        if (prev < 1e-8) continue;
        sum += (cur - prev) / prev;
        ++count;
    }
    return (count > 0) ? (sum / count) : 0.0;
}

double BPGVRotationStrategy::compute_sigma_percentile(int sigma_window, int percentile_window) const {
    // Rank today's sigma_{sigma_window} among the trailing `percentile_window`
    // daily sigma values. Returns a value in [0, 1].
    int n = static_cast<int>(portfolio_value_history_.size());
    int min_needed = sigma_window + percentile_window;
    if (n < min_needed) return 1.0;  // not enough history — fail-open (allow triggers)

    // Current sigma.
    double current_sigma = compute_daily_return_stdev(sigma_window);

    // Walk back `percentile_window` days and count how many trailing sigmas
    // were <= current_sigma. Computing a fresh sigma at every offset is O(W^2)
    // but this runs once per day and percentile_window ~ 504, so ~250k ops/day
    // — fine for a daily strategy.
    int below_or_equal = 0;
    int total = 0;
    for (int offset = 1; offset <= percentile_window; ++offset) {
        int end = n - offset;
        int start = end - sigma_window;
        if (start < 1) break;
        // Compute stdev over [start, end).
        std::vector<double> rets;
        rets.reserve(sigma_window);
        for (int i = start; i < end; ++i) {
            double prev = portfolio_value_history_[i - 1];
            double cur = portfolio_value_history_[i];
            if (prev < 1e-8) { rets.push_back(0.0); continue; }
            rets.push_back((cur - prev) / prev);
        }
        if (rets.empty()) continue;
        double mean = 0.0;
        for (double r : rets) mean += r;
        mean /= static_cast<double>(rets.size());
        double var = 0.0;
        for (double r : rets) var += (r - mean) * (r - mean);
        var /= static_cast<double>(rets.size() - 1);
        double sig = std::sqrt(var);
        if (sig <= current_sigma) ++below_or_equal;
        ++total;
    }
    return (total > 0) ? (static_cast<double>(below_or_equal) / total) : 1.0;
}

// --- Crash detection --------------------------------------------------------

bool BPGVRotationStrategy::detect_crash() const {
    const auto& tcfg = bpgv_config_.crash_override.trigger;

    if (tcfg.method == "fixed_drawdown") {
        // Baseline path — retained for A/B and back-compat.
        int needed = bpgv_config_.crash_lookback_days + 1;
        if (static_cast<int>(portfolio_value_history_.size()) < needed) return false;
        double current = portfolio_value_history_.back();
        double lookback = portfolio_value_history_[portfolio_value_history_.size() - needed];
        if (lookback < 1e-8) return false;
        double drawdown = (current - lookback) / lookback;
        return drawdown < bpgv_config_.crash_threshold;
    }

    if (tcfg.method == "volatility_scaled") {
        // Z-score rule with vol-state gate (Moreira-Muir 2017, Harvey et al. 2018).
        int n = static_cast<int>(portfolio_value_history_.size());
        if (n < std::max(tcfg.min_history_days,
                         bpgv_config_.crash_lookback_days + 1))
            return false;

        // 5-day realized portfolio return.
        int back_idx = n - 1 - bpgv_config_.crash_lookback_days;
        if (back_idx < 0) return false;
        double current = portfolio_value_history_.back();
        double lookback = portfolio_value_history_[back_idx];
        if (lookback < 1e-8) return false;
        double r_nd = (current - lookback) / lookback;

        // 60-day realized daily-return sigma, scaled to crash_lookback_days.
        double sigma_60 = compute_daily_return_stdev(tcfg.lookback_days);
        double mu_60 = compute_daily_return_mean(tcfg.lookback_days);
        if (sigma_60 < 1e-10) return false;
        double sigma_nd = sigma_60 * std::sqrt(static_cast<double>(bpgv_config_.crash_lookback_days));
        double mu_nd = mu_60 * bpgv_config_.crash_lookback_days;

        double z = (r_nd - mu_nd) / sigma_nd;

        // Vol-state gate: only fire when current sigma is in the upper
        // (1 - vol_gate_percentile) tail of its own recent history. Prevents
        // false positives in compressed-vol regimes.
        double sigma_pct = compute_sigma_percentile(tcfg.lookback_days, tcfg.vol_gate_window);
        bool vol_gate = (sigma_pct > tcfg.vol_gate_percentile);

        return (z < -tcfg.k_sigma) && vol_gate;
    }

    WARN("Unknown crash trigger method: " + tcfg.method +
         ". Falling through to no-crash (safe default).");
    return false;
}

void BPGVRotationStrategy::activate_crash_override(const Timestamp& ts) {
    crash_override_active_ = true;
    crash_override_start_ = ts;
    crash_override_count_++;
    consecutive_good_exit_days_ = 0;  // Change 4: reset on activation
    INFO("CRASH OVERRIDE activated (count: " + std::to_string(crash_override_count_) +
         ") — switching to defensive allocation. Exit method: " +
         bpgv_config_.crash_override.exit.method);
}

// --- Change 4 helpers -------------------------------------------------------

double BPGVRotationStrategy::compute_exit_score(const std::deque<double>& trend_px) const {
    const auto& ecfg = bpgv_config_.crash_override.exit;

    // Need at least 60 days of trend history and 60 of portfolio history to
    // score all three components. If insufficient, return 0 (don't exit yet).
    if (static_cast<int>(trend_px.size()) < 60) return 0.0;
    if (portfolio_value_history_.size() < 60) return 0.0;

    // Trend component: close > SMA_20 AND SMA_10 > SMA_30.
    double sma_20 = calculate_sma(trend_px, 20);
    double sma_10 = calculate_sma(trend_px, 10);
    double sma_30 = calculate_sma(trend_px, 30);
    double last_close = trend_px.back();
    double trend = (last_close > sma_20 && sma_10 > sma_30) ? 1.0 : 0.0;

    // Vol-norm component: sigma_20 / sigma_60 < 1.2.
    // Compute on trend_px daily returns.
    auto compute_sigma = [&](int window) {
        int n = static_cast<int>(trend_px.size());
        if (n < window + 1) return 0.0;
        std::vector<double> rets;
        rets.reserve(window);
        for (int i = n - window; i < n; ++i) {
            double prev = trend_px[i - 1];
            double cur = trend_px[i];
            if (prev < 1e-8) { rets.push_back(0.0); continue; }
            rets.push_back((cur - prev) / prev);
        }
        double mean = 0.0;
        for (double r : rets) mean += r;
        mean /= rets.size();
        double var = 0.0;
        for (double r : rets) var += (r - mean) * (r - mean);
        var /= (rets.size() - 1);
        return std::sqrt(var);
    };
    double sigma_20 = compute_sigma(20);
    double sigma_60 = compute_sigma(60);
    double vol_norm = 0.0;
    if (sigma_60 > 1e-10) {
        vol_norm = ((sigma_20 / sigma_60) < 1.2) ? 1.0 : 0.0;
    }

    // Recovery component: portfolio within 3 % of its trailing 60-day max.
    double max_60 = 0.0;
    int pn = static_cast<int>(portfolio_value_history_.size());
    for (int i = pn - 60; i < pn; ++i) {
        if (portfolio_value_history_[i] > max_60) max_60 = portfolio_value_history_[i];
    }
    double recov = 0.0;
    if (max_60 > 1e-8) {
        recov = (portfolio_value_history_.back() > 0.97 * max_60) ? 1.0 : 0.0;
    }

    return ecfg.weight_trend * trend
         + ecfg.weight_vol_norm * vol_norm
         + ecfg.weight_recov * recov;
}

bool BPGVRotationStrategy::is_crash_override_expired(const Timestamp& ts) const {
    const auto& ecfg = bpgv_config_.crash_override.exit;

    auto duration = ts - crash_override_start_;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(duration).count();
    auto calendar_days = static_cast<int>(hours / 24);

    if (ecfg.method == "calendar_timer") {
        int days = bpgv_config_.crash_override_calendar_days;
        return hours >= (days * 24);
    }

    if (ecfg.method == "signal_contingent") {
        // Honor minimum hold.
        if (calendar_days < ecfg.min_hold_days) return false;

        // Optional hard cap.
        if (ecfg.max_hold_days > 0 && calendar_days >= ecfg.max_hold_days) {
            return true;
        }

        // Need consecutive_good_exit_days_ >= confirmation_days. Note this
        // state is advanced in on_data() — is_crash_override_expired() is a
        // pure-read query here.
        return consecutive_good_exit_days_ >= ecfg.confirmation_days;
    }

    WARN("Unknown exit method: " + ecfg.method +
         ". Falling through to calendar_timer default.");
    return hours >= (bpgv_config_.crash_override_calendar_days * 24);
}

std::unordered_map<std::string, double> BPGVRotationStrategy::build_crash_weights() const {
    // Tier-1 defensive basket: config-driven via crash_override.defensive_weights.
    // Default prompt basket: {BIL:0.40, TLT:0.25, GLD:0.20, DBMF:0.15}.
    //
    // DBMF (inception 2019-05) may not have a live price early in the backtest;
    // when that happens we splice its weight into `splice_fallback_symbol` (BIL)
    // so the basket still sums to 1.0. All `zero_symbols` are forced to 0.0 so
    // the override can't ride the very assets it is meant to hedge against
    // (the baseline bug: SPY was 32 % of the old defensive basket and the
    // single largest realized loser at −$34 k over 15 y).

    std::unordered_map<std::string, double> weights;

    // Initialize full universe to zero so any symbol not in the basket
    // contributes nothing.
    for (const auto& sym : bpgv_config_.risk_on_symbols) weights[sym] = 0.0;
    for (const auto& sym : bpgv_config_.risk_off_symbols) weights[sym] = 0.0;

    const auto& cfg = bpgv_config_.crash_override;

    // Config-driven path (Tier-1). Fails loudly if defensive_weights is empty:
    // guardrail #4 (no silent fallback to hardcoded basket).
    if (cfg.defensive_weights.empty()) {
        ERROR("BPGV crash override: crash_override.defensive_weights is empty. "
              "Tier-1 requires an explicit defensive basket in config.");
        return weights;  // all zeros — portfolio will hold cash-equivalent via execution layer
    }

    // Splice logic: accumulate weights of defensive symbols that have no live
    // price into fallback_weight, then add it to splice_fallback_symbol.
    double fallback_weight = 0.0;
    for (const auto& [sym, w] : cfg.defensive_weights) {
        auto state_it = symbol_state_.find(sym);
        bool has_price = (state_it != symbol_state_.end()
                          && state_it->second.current_price > 1e-8
                          && !state_it->second.price_history.empty());
        if (has_price) {
            weights[sym] = w;
        } else {
            fallback_weight += w;
        }
    }

    if (fallback_weight > 1e-8) {
        // Splice into fallback symbol. If the fallback also has no price, log
        // a warning — the override will still zero everything else out.
        auto fb_it = symbol_state_.find(cfg.splice_fallback_symbol);
        bool fb_has_price = (fb_it != symbol_state_.end()
                             && fb_it->second.current_price > 1e-8
                             && !fb_it->second.price_history.empty());
        if (fb_has_price) {
            weights[cfg.splice_fallback_symbol] += fallback_weight;
            DEBUG("BPGV crash override: spliced " + std::to_string(fallback_weight) +
                  " of missing-price weight into " + cfg.splice_fallback_symbol);
        } else {
            WARN("BPGV crash override: splice fallback symbol " +
                 cfg.splice_fallback_symbol + " has no price; " +
                 std::to_string(fallback_weight) + " of weight unallocated");
        }
    }

    // Force zero on all configured zero_symbols (e.g. SPY/QQQ/XLK/SMH/IWM/XHB/IYR/EQR).
    for (const auto& sym : cfg.zero_symbols) {
        weights[sym] = 0.0;
    }

    return weights;
}

// ============================================================================
// Position management
// ============================================================================

double BPGVRotationStrategy::compute_portfolio_value() const {
    double value = 0.0;
    bool has_positions = false;

    for (const auto& [sym, pos] : positions_) {
        double qty = pos.quantity.as_double();
        if (std::abs(qty) < 1e-8) continue;

        has_positions = true;
        auto state_it = symbol_state_.find(sym);
        if (state_it != symbol_state_.end() && state_it->second.current_price > 0.0) {
            value += qty * state_it->second.current_price;
        }
    }

    return has_positions ? value : 0.0;
}

void BPGVRotationStrategy::update_positions_from_weights() {
    // Validate live prices for symbols with non-trivial weight. Targets themselves
    // are materialized in get_target_positions(); direct writes to positions_ are
    // forbidden here — positions_ is owned by on_execution, so writing would
    // double-count once the portfolio fires the resulting fill back at us.
    const double sizing_capital = static_cast<double>(config_.capital_allocation);
    (void)sizing_capital;

    for (const auto& [sym, weight] : current_weights_) {
        if (weight <= 1e-8) continue;
        auto state_it = symbol_state_.find(sym);
        double price = (state_it != symbol_state_.end()) ? state_it->second.current_price : 0.0;
        if (price <= 1e-8) {
            ERROR("BPGV: cannot size " + sym + " — current_price=" +
                  std::to_string(price) + ", target_weight=" + std::to_string(weight) +
                  ". Target will be zero until price data arrives.");
        }
    }
}

std::unordered_map<std::string, Position> BPGVRotationStrategy::get_target_positions() const {
    std::unordered_map<std::string, Position> targets;

    // Fixed initial capital for sizing — mirrors MeanReversionStrategy pattern and
    // avoids the doom loop where cost drag pushes live NAV negative.
    const double sizing_capital = static_cast<double>(config_.capital_allocation);

    // Build an entry for every symbol in the universe so that dropped symbols show
    // up as qty=0 and the portfolio exec path can issue explicit close orders.
    // Include cash symbols so BIL/DBMF targets are emitted during crash override
    // and cleanly zeroed when the override ends.
    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.cash_symbols.begin(),
                       bpgv_config_.cash_symbols.end());

    // Tier-1 Change 2: Masters (2003) halfway-rule tolerance-band rebalancing.
    //
    // Baseline bug: every daily price tick recomputed target_shares = w × C / p,
    // the portfolio saw a diff, and the executor emitted a tiny trim fill. This
    // accounted for ~250 trade-days/year vs. the ~26 the strategy spec intended,
    // costing ~1.3 %/yr in commissions + slippage.
    //
    // Fix: compare w_target to w_current (realized). Only re-size when drift
    // exceeds |100 bps| absolute OR |25 %| relative, and when triggered, trade
    // only halfway back to target (Masters' concave-benefit / linear-cost
    // argument: a half-sized trade captures most of the rebalancing benefit at
    // half the cost). Full exits on zero-target and full entries from zero
    // remain decisive so override entry/exit is not partial.
    const auto& rcfg = bpgv_config_.rebalance;

    for (const auto& sym : all_symbols) {
        Position pos;
        pos.symbol = sym;
        pos.quantity = 0.0;

        auto w_it = current_weights_.find(sym);
        double w_target = (w_it != current_weights_.end()) ? w_it->second : 0.0;
        if (w_target < 0.0) w_target = 0.0;

        auto state_it = symbol_state_.find(sym);
        double price = (state_it != symbol_state_.end()) ? state_it->second.current_price : 0.0;

        // Current realized position and weight.
        double current_qty = 0.0;
        auto pos_it = positions_.find(sym);
        if (pos_it != positions_.end()) {
            current_qty = pos_it->second.quantity.as_double();
        }
        double current_notional = current_qty * price;
        double w_current = (sizing_capital > 1e-8) ? (current_notional / sizing_capital) : 0.0;

        // Decide effective weight via tolerance band.
        double w_effective = w_current;  // default: no trade

        const bool want_zero = (w_target <= 1e-8);
        const bool have_none = (std::abs(w_current) <= 1e-8);

        if (want_zero && !have_none && rcfg.exit_fully_on_zero_target) {
            w_effective = 0.0;
        } else if (!want_zero && have_none && rcfg.enter_fully_on_zero_current) {
            w_effective = w_target;
        } else if (!want_zero) {
            double drift_abs = w_current - w_target;
            double drift_rel = drift_abs / w_target;
            bool trigger = (std::abs(drift_abs) > rcfg.drift_abs_trigger) ||
                           (std::abs(drift_rel) > rcfg.drift_rel_trigger);
            if (trigger) {
                w_effective = rcfg.halfway_rule
                                  ? (w_current - 0.5 * drift_abs)  // halfway back to target
                                  : w_target;
            }
        }
        // else: both zero → no trade, w_effective stays at w_current (= 0).

        // Materialize target shares.
        if (w_effective > 1e-8 && price > 1e-8) {
            double target_shares = (w_effective * sizing_capital) / price;
            if (!bpgv_config_.allow_fractional_shares) {
                target_shares = std::floor(target_shares);
            }
            pos.quantity = Quantity(target_shares);
        } else if (w_effective <= 1e-8) {
            pos.quantity = Quantity(0.0);
        }
        // If price is 0 but we want non-zero weight, leave qty=0 (don't size against stale price).

        // Preserve live cost basis / PnL from the fill-driven positions_ map.
        if (pos_it != positions_.end()) {
            pos.average_price = pos_it->second.average_price;
            pos.realized_pnl = pos_it->second.realized_pnl;
            pos.unrealized_pnl = pos_it->second.unrealized_pnl;
            pos.last_update = pos_it->second.last_update;
        }

        targets[sym] = pos;
    }

    return targets;
}

// ============================================================================
// Helper calculations
// ============================================================================

double BPGVRotationStrategy::calculate_sma(const std::deque<double>& prices, int period) const {
    if (prices.empty() || period <= 0 || static_cast<int>(prices.size()) < period) {
        return 0.0;
    }

    double sum = 0.0;
    for (size_t i = prices.size() - period; i < prices.size(); ++i) {
        sum += prices[i];
    }
    return sum / period;
}

double BPGVRotationStrategy::calculate_trailing_return(
    const std::deque<double>& prices, int days) const {

    if (static_cast<int>(prices.size()) < days + 1) return 0.0;

    double current = prices.back();
    double past = prices[prices.size() - days - 1];
    if (past < 1e-8) return 0.0;

    return (current - past) / past;
}

std::unordered_map<std::string, std::vector<double>> BPGVRotationStrategy::get_price_history() const {
    std::unordered_map<std::string, std::vector<double>> history;
    for (const auto& [sym, state] : symbol_state_) {
        history[sym] = std::vector<double>(state.price_history.begin(),
                                            state.price_history.end());
    }
    return history;
}

BPGVRotationStrategy::DateParts BPGVRotationStrategy::extract_date(const Timestamp& ts) {
    auto time_t = std::chrono::system_clock::to_time_t(ts);
    std::tm* tm = std::localtime(&time_t);
    return {tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday};
}

}  // namespace trade_ngin
