// src/strategy/bpgv_rotation.cpp
#include "trade_ngin/strategy/bpgv_rotation.hpp"
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include "trade_ngin/core/time_utils.hpp"

namespace trade_ngin {

// ============================================================================
// Config parsing
// ============================================================================

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

void VolTargetConfig::from_json(const nlohmann::json& j) {
    if (j.contains("enabled")) enabled = j.at("enabled").get<bool>();
    if (j.contains("target_annualized"))
        target_annualized = j.at("target_annualized").get<double>();
    if (j.contains("window_days")) window_days = j.at("window_days").get<int>();
    if (j.contains("scalar_floor")) scalar_floor = j.at("scalar_floor").get<double>();
    if (j.contains("min_history_days"))
        min_history_days = j.at("min_history_days").get<int>();
    if (j.contains("enable_dynamic_target"))
        enable_dynamic_target = j.at("enable_dynamic_target").get<bool>();
    if (j.contains("target_annualized_min"))
        target_annualized_min = j.at("target_annualized_min").get<double>();
    if (j.contains("target_annualized_max"))
        target_annualized_max = j.at("target_annualized_max").get<double>();
    if (j.contains("pctile_high"))
        pctile_high = j.at("pctile_high").get<double>();
    if (j.contains("pctile_low"))
        pctile_low = j.at("pctile_low").get<double>();
    if (j.contains("use_garch_natural_vol"))
        use_garch_natural_vol = j.at("use_garch_natural_vol").get<bool>();
    if (j.contains("garch_natural_vol_multiplier"))
        garch_natural_vol_multiplier = j.at("garch_natural_vol_multiplier").get<double>();
    if (j.contains("use_max_risk_sigma"))
        use_max_risk_sigma = j.at("use_max_risk_sigma").get<bool>();
    if (j.contains("conditional_max_risk_sigma"))
        conditional_max_risk_sigma = j.at("conditional_max_risk_sigma").get<bool>();
    if (j.contains("max_risk_on_market_stress"))
        max_risk_on_market_stress = j.at("max_risk_on_market_stress").get<bool>();
    if (j.contains("max_risk_on_breadth_stress"))
        max_risk_on_breadth_stress = j.at("max_risk_on_breadth_stress").get<bool>();
    if (j.contains("include_garch_sigma"))
        include_garch_sigma = j.at("include_garch_sigma").get<bool>();
    if (j.contains("include_realized_portfolio_sigma"))
        include_realized_portfolio_sigma = j.at("include_realized_portfolio_sigma").get<bool>();
    if (j.contains("include_stress_symbol_sigma"))
        include_stress_symbol_sigma = j.at("include_stress_symbol_sigma").get<bool>();
    if (j.contains("include_defensive_pair_sigma"))
        include_defensive_pair_sigma = j.at("include_defensive_pair_sigma").get<bool>();
    if (j.contains("stress_vol_symbol"))
        stress_vol_symbol = j.at("stress_vol_symbol").get<std::string>();
    if (j.contains("stress_vol_window_days"))
        stress_vol_window_days = j.at("stress_vol_window_days").get<int>();
    if (j.contains("defensive_pair_vol_window_days"))
        defensive_pair_vol_window_days = j.at("defensive_pair_vol_window_days").get<int>();
}

void BreadthRiskConfig::from_json(const nlohmann::json& j) {
    if (j.contains("enabled")) enabled = j.at("enabled").get<bool>();
    if (j.contains("symbols"))
        symbols = j.at("symbols").get<std::vector<std::string>>();
    if (j.contains("momentum_lookback_days"))
        momentum_lookback_days = j.at("momentum_lookback_days").get<int>();
    if (j.contains("sma_window"))
        sma_window = j.at("sma_window").get<int>();
    if (j.contains("risk_free_symbol"))
        risk_free_symbol = j.at("risk_free_symbol").get<std::string>();
    if (j.contains("require_positive_excess"))
        require_positive_excess = j.at("require_positive_excess").get<bool>();
    if (j.contains("require_above_sma"))
        require_above_sma = j.at("require_above_sma").get<bool>();
    if (j.contains("full_risk_breadth"))
        full_risk_breadth = j.at("full_risk_breadth").get<double>();
    if (j.contains("stress_breadth"))
        stress_breadth = j.at("stress_breadth").get<double>();
    if (j.contains("risk_on_floor"))
        risk_on_floor = j.at("risk_on_floor").get<double>();
    if (j.contains("splice_symbol"))
        splice_symbol = j.at("splice_symbol").get<std::string>();
    if (j.contains("min_history_days"))
        min_history_days = j.at("min_history_days").get<int>();
    if (j.contains("fail_open_on_short_history"))
        fail_open_on_short_history = j.at("fail_open_on_short_history").get<bool>();
}

void DefensiveSleeveConfig::from_json(const nlohmann::json& j) {
    if (j.contains("vol_aware_enabled"))
        vol_aware_enabled = j.at("vol_aware_enabled").get<bool>();
    if (j.contains("pair_vol_target_annualized"))
        pair_vol_target_annualized = j.at("pair_vol_target_annualized").get<double>();
    if (j.contains("vol_window_days"))
        vol_window_days = j.at("vol_window_days").get<int>();
    if (j.contains("use_inverse_vol_pair"))
        use_inverse_vol_pair = j.at("use_inverse_vol_pair").get<bool>();
    if (j.contains("splice_symbol"))
        splice_symbol = j.at("splice_symbol").get<std::string>();
    if (j.contains("max_splice_fraction"))
        max_splice_fraction = j.at("max_splice_fraction").get<double>();
    if (j.contains("min_history_days"))
        min_history_days = j.at("min_history_days").get<int>();
    if (j.contains("garch_aware_enabled"))
        garch_aware_enabled = j.at("garch_aware_enabled").get<bool>();
    if (j.contains("pair_vol_target_high_sigma"))
        pair_vol_target_high_sigma = j.at("pair_vol_target_high_sigma").get<double>();
    if (j.contains("pair_vol_target_low_sigma"))
        pair_vol_target_low_sigma = j.at("pair_vol_target_low_sigma").get<double>();
    if (j.contains("garch_pctile_high"))
        garch_pctile_high = j.at("garch_pctile_high").get<double>();
    if (j.contains("garch_pctile_low"))
        garch_pctile_low = j.at("garch_pctile_low").get<double>();
}

void MarketStressConfig::from_json(const nlohmann::json& j) {
    if (j.contains("enabled")) enabled = j.at("enabled").get<bool>();
    if (j.contains("stress_symbol"))
        stress_symbol = j.at("stress_symbol").get<std::string>();
    if (j.contains("drawdown_lookback_days"))
        drawdown_lookback_days = j.at("drawdown_lookback_days").get<int>();
    if (j.contains("drawdown_trigger"))
        drawdown_trigger = j.at("drawdown_trigger").get<double>();
    if (j.contains("sma_window")) sma_window = j.at("sma_window").get<int>();
    if (j.contains("risk_on_scale")) risk_on_scale = j.at("risk_on_scale").get<double>();
    if (j.contains("min_history_days"))
        min_history_days = j.at("min_history_days").get<int>();
}

void BPGGrowthSignalConfig::from_json(const nlohmann::json& j) {
    if (j.contains("enabled")) enabled = j.at("enabled").get<bool>();
    if (j.contains("lookback_months"))
        lookback_months = j.at("lookback_months").get<int>();
    if (j.contains("min_history_months"))
        min_history_months = j.at("min_history_months").get<int>();
    if (j.contains("use_sfh_bpg"))
        use_sfh_bpg = j.at("use_sfh_bpg").get<bool>();
    if (j.contains("growth_scale"))
        growth_scale = j.at("growth_scale").get<double>();
    if (j.contains("max_risk_off_shift"))
        max_risk_off_shift = j.at("max_risk_off_shift").get<double>();
}

void HousingExposureConfig::from_json(const nlohmann::json& j) {
    if (j.contains("enabled")) enabled = j.at("enabled").get<bool>();
    if (j.contains("symbols"))
        symbols = j.at("symbols").get<std::vector<std::string>>();
    if (j.contains("garch_percentile_trigger"))
        garch_percentile_trigger = j.at("garch_percentile_trigger").get<double>();
    if (j.contains("growth_signal_trigger"))
        growth_signal_trigger = j.at("growth_signal_trigger").get<double>();
    if (j.contains("cap_scale"))
        cap_scale = j.at("cap_scale").get<double>();
    if (j.contains("splice_symbol"))
        splice_symbol = j.at("splice_symbol").get<std::string>();
}

void DurationDefenseConfig::from_json(const nlohmann::json& j) {
    if (j.contains("enabled")) enabled = j.at("enabled").get<bool>();
    if (j.contains("duration_symbol"))
        duration_symbol = j.at("duration_symbol").get<std::string>();
    if (j.contains("splice_symbol"))
        splice_symbol = j.at("splice_symbol").get<std::string>();
    if (j.contains("trend_lookback_days"))
        trend_lookback_days = j.at("trend_lookback_days").get<int>();
    if (j.contains("sma_window"))
        sma_window = j.at("sma_window").get<int>();
    if (j.contains("guard_scale"))
        guard_scale = j.at("guard_scale").get<double>();
    if (j.contains("require_negative_return"))
        require_negative_return = j.at("require_negative_return").get<bool>();
    if (j.contains("require_below_sma"))
        require_below_sma = j.at("require_below_sma").get<bool>();
}

void BreakoutConfig::from_json(const nlohmann::json& j) {
    if (j.contains("sma_window")) sma_window = j.at("sma_window").get<int>();
    if (j.contains("atr_window")) atr_window = j.at("atr_window").get<int>();
    if (j.contains("atr_k")) atr_k = j.at("atr_k").get<double>();
    if (j.contains("confirmation_days"))
        confirmation_days = j.at("confirmation_days").get<int>();
    if (j.contains("graded_weighting"))
        graded_weighting = j.at("graded_weighting").get<bool>();
    if (j.contains("fail_open_on_short_history"))
        fail_open_on_short_history = j.at("fail_open_on_short_history").get<bool>();
}

BPGVRotationConfig BPGVRotationConfig::from_json(const nlohmann::json& cfg) {
    BPGVRotationConfig c;
    c.source_json = cfg;

    c.macro_csv_path = cfg.value("macro_csv_path", c.macro_csv_path);
    c.rebalance_day_of_month = cfg.value("rebalance_day_of_month", c.rebalance_day_of_month);
    c.base_risk_off_min = cfg.value("base_risk_off_min", c.base_risk_off_min);
    c.base_risk_off_range = cfg.value("base_risk_off_range", c.base_risk_off_range);
    c.strong_risk_on_equity_boost =
        cfg.value("strong_risk_on_equity_boost", c.strong_risk_on_equity_boost);
    c.strong_risk_on_bond_reduction =
        cfg.value("strong_risk_on_bond_reduction", c.strong_risk_on_bond_reduction);
    c.homebuilder_tilt_scale = cfg.value("homebuilder_tilt_scale", c.homebuilder_tilt_scale);
    c.homebuilder_symbol = cfg.value("homebuilder_symbol", c.homebuilder_symbol);
    c.allow_fractional_shares = cfg.value("allow_fractional_shares", c.allow_fractional_shares);

    auto load_symbols = [&cfg](const char* key, std::vector<std::string>& out) {
        if (!cfg.contains(key)) return;
        out.clear();
        for (const auto& sym : cfg.at(key)) out.push_back(sym.get<std::string>());
    };
    load_symbols("risk_on_symbols", c.risk_on_symbols);
    load_symbols("risk_off_symbols", c.risk_off_symbols);
    load_symbols("cash_symbols", c.cash_symbols);

    if (cfg.contains("rebalance")) c.rebalance.from_json(cfg.at("rebalance"));
    if (cfg.contains("momentum")) c.momentum.from_json(cfg.at("momentum"));
    if (cfg.contains("breakout")) c.breakout.from_json(cfg.at("breakout"));
    if (cfg.contains("vol_target")) c.vol_target.from_json(cfg.at("vol_target"));
    if (cfg.contains("defensive_sleeve")) c.defensive_sleeve.from_json(cfg.at("defensive_sleeve"));
    if (cfg.contains("market_stress")) c.market_stress.from_json(cfg.at("market_stress"));
    if (cfg.contains("breadth_risk")) c.breadth_risk.from_json(cfg.at("breadth_risk"));
    if (cfg.contains("bpg_growth_signal"))
        c.bpg_growth_signal.from_json(cfg.at("bpg_growth_signal"));
    if (cfg.contains("housing_exposure")) c.housing_exposure.from_json(cfg.at("housing_exposure"));
    if (cfg.contains("duration_defense")) c.duration_defense.from_json(cfg.at("duration_defense"));
    return c;
}

nlohmann::json BPGVRotationConfig::to_json() const {
    nlohmann::json j;
    j["macro_csv_path"] = macro_csv_path;
    j["rebalance_day_of_month"] = rebalance_day_of_month;
    j["base_risk_off_min"] = base_risk_off_min;
    j["base_risk_off_range"] = base_risk_off_range;
    j["strong_risk_on_equity_boost"] = strong_risk_on_equity_boost;
    j["strong_risk_on_bond_reduction"] = strong_risk_on_bond_reduction;
    j["homebuilder_tilt_scale"] = homebuilder_tilt_scale;
    j["homebuilder_symbol"] = homebuilder_symbol;
    j["allow_fractional_shares"] = allow_fractional_shares;
    j["risk_on_symbols"] = risk_on_symbols;
    j["risk_off_symbols"] = risk_off_symbols;
    j["cash_symbols"] = cash_symbols;
    j["as_configured"] = source_json.is_null() ? nlohmann::json::object() : source_json;
    return j;
}

int BPGVRotationConfig::required_warmup_days() const {
    int days = std::max({momentum.tsm_lookback_days, momentum.xsec_lookback_days,
                         momentum.xsec_vol_window_days, breakout.sma_window, breakout.atr_window});
    if (breadth_risk.enabled) {
        days = std::max({days, breadth_risk.momentum_lookback_days + 1, breadth_risk.sma_window,
                         breadth_risk.min_history_days});
    }
    return days;
}

// ============================================================================
// Construction and validation
// ============================================================================

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

    auto invalid = [](const std::string& message) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT, message, "BPGVRotationStrategy");
    };

    if (bpgv_config_.rebalance_day_of_month < 1 || bpgv_config_.rebalance_day_of_month > 28) {
        return invalid("Rebalance day must be between 1 and 28");
    }
    if (bpgv_config_.risk_on_symbols.empty()) {
        return invalid("Risk-on symbols list cannot be empty");
    }
    if (bpgv_config_.risk_off_symbols.empty()) {
        return invalid("Risk-off symbols list cannot be empty");
    }

    if (bpgv_config_.market_stress.enabled) {
        const auto& cfg = bpgv_config_.market_stress;
        if (cfg.drawdown_lookback_days <= 0 || cfg.sma_window <= 1 || cfg.min_history_days <= 1) {
            return invalid("market_stress lookbacks must be positive");
        }
        if (cfg.drawdown_trigger >= 0.0) {
            return invalid("market_stress.drawdown_trigger must be negative");
        }
        if (cfg.risk_on_scale < 0.0 || cfg.risk_on_scale > 1.0) {
            return invalid("market_stress.risk_on_scale must be in [0, 1]");
        }
    }

    if (bpgv_config_.vol_target.enabled && bpgv_config_.vol_target.use_max_risk_sigma) {
        const auto& cfg = bpgv_config_.vol_target;
        if (cfg.stress_vol_window_days <= 1 || cfg.defensive_pair_vol_window_days <= 1) {
            return invalid("vol_target max-risk windows must be > 1");
        }
    }

    if (bpgv_config_.breadth_risk.enabled) {
        const auto& cfg = bpgv_config_.breadth_risk;
        if (cfg.momentum_lookback_days <= 1 || cfg.sma_window <= 1 ||
            cfg.min_history_days <= 1) {
            return invalid("breadth_risk lookbacks must be > 1");
        }
        if (cfg.full_risk_breadth <= 0.0 || cfg.full_risk_breadth > 1.0 ||
            cfg.stress_breadth < 0.0 || cfg.stress_breadth > 1.0 ||
            cfg.risk_on_floor < 0.0 || cfg.risk_on_floor > 1.0) {
            return invalid("breadth_risk breadth/floor values must be in valid [0, 1] ranges");
        }
    }

    if (bpgv_config_.bpg_growth_signal.enabled) {
        const auto& cfg = bpgv_config_.bpg_growth_signal;
        if (cfg.lookback_months <= 0 || cfg.min_history_months <= 0 ||
            cfg.min_history_months > cfg.lookback_months) {
            return invalid("bpg_growth_signal history settings are invalid");
        }
        if (cfg.growth_scale <= 0.0 || cfg.max_risk_off_shift < 0.0 ||
            cfg.max_risk_off_shift > 0.50) {
            return invalid("bpg_growth_signal scale/shift settings are invalid");
        }
    }

    if (bpgv_config_.housing_exposure.enabled) {
        const auto& cfg = bpgv_config_.housing_exposure;
        if (cfg.symbols.empty()) {
            return invalid("housing_exposure.symbols cannot be empty");
        }
        if (cfg.cap_scale < 0.0 || cfg.cap_scale > 1.0) {
            return invalid("housing_exposure.cap_scale must be in [0, 1]");
        }
    }

    if (bpgv_config_.duration_defense.enabled) {
        const auto& cfg = bpgv_config_.duration_defense;
        if (cfg.trend_lookback_days <= 1 || cfg.sma_window <= 1) {
            return invalid("duration_defense lookbacks must be > 1");
        }
        if (cfg.guard_scale < 0.0 || cfg.guard_scale > 1.0) {
            return invalid("duration_defense.guard_scale must be in [0, 1]");
        }
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

    // State and a position row for every symbol, cash symbols included: BIL is the
    // risk-free reference and the splice target.
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

    // History arrives through on_data: the backtest coordinator treats the first
    // required_warmup_days() bars as warmup over one continuous adjusted series.
    INFO("BPGV rotation strategy initialized for " + std::to_string(all_symbols.size()) + " symbols");
    return Result<void>();
}

// ============================================================================
// Daily cycle
// ============================================================================

Result<void> BPGVRotationStrategy::on_data(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "Strategy is not in running state",
                                "BPGVRotationStrategy");
    }
    if (data.empty()) return Result<void>();

    // One rebalance decision per UTC day, in order, however many days a call carries.
    std::map<int, std::vector<Bar>> by_day;
    for (const auto& bar : data) {
        const auto d = extract_date(bar.timestamp);
        by_day[d.year * 10000 + d.month * 100 + d.day].push_back(bar);
    }
    if (by_day.size() == 1) return on_day(data);
    for (auto& [day_key, day_bars] : by_day) {
        (void)day_key;
        auto r = on_day(day_bars);
        if (r.is_error()) return r;
    }
    return Result<void>();
}

Result<void> BPGVRotationStrategy::on_day(const std::vector<Bar>& data) {
    if (state_ != StrategyState::RUNNING) {
        return make_error<void>(ErrorCode::STRATEGY_ERROR,
                                "Strategy is not in running state",
                                "BPGVRotationStrategy");
    }
    if (data.empty()) return Result<void>();

    try {
        update_price_histories(data);

        // The holdings' value feeds the realized-volatility estimate; a flat book keeps the
        // last value. Only the volatility target's window (plus slack) is kept.
        double pv = compute_portfolio_value();
        if (pv > 0.0) {
            portfolio_value_ = pv;
        }
        portfolio_value_history_.push_back(portfolio_value_);
        const size_t needed = static_cast<size_t>(bpgv_config_.vol_target.window_days + 10);
        while (portfolio_value_history_.size() > needed) {
            portfolio_value_history_.pop_front();
        }

        auto date = extract_date(data.front().timestamp);
        if (should_rebalance(date.year, date.month, date.day)) {
            execute_rebalance(date.year, date.month);
        }

        // Re-applied from the last rebalance weights, so the haircut does not compound.
        refresh_market_stress_overlay();

        // Timestamps only: on_execution owns the cost basis.
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
    // The previous month's record: permits are released mid-month.
    int prev_year = (month == 1) ? year - 1 : year;
    int prev_month = (month == 1) ? 12 : month - 1;

    auto record = MacroCSVLoader::find_record(macro_data_, prev_year, prev_month);
    if (!record.has_value()) {
        record = MacroCSVLoader::find_record_before(macro_data_, prev_year, prev_month);
        if (!record.has_value()) {
            WARN("No macro data available for rebalance at " +
                 std::to_string(year) + "-" + std::to_string(month) + ", skipping");
            return;
        }
        WARN("Using fallback macro data from " +
             std::to_string(record->year) + "-" + std::to_string(record->month));
    }

    // The month's volatility target and GARCH forecast; without a GARCH value the static
    // target and the realized-volatility path apply.
    {
        const auto& vt = bpgv_config_.vol_target;
        double eff = vt.target_annualized;
        if (vt.enable_dynamic_target && record->bpgv_garch > 0.0) {
            double pct = record->bpgv_garch_percentile;
            if (pct >= vt.pctile_high) {
                eff = vt.target_annualized_min;
            } else if (pct <= vt.pctile_low) {
                eff = vt.target_annualized_max;
            } else {
                double t = (pct - vt.pctile_low) / (vt.pctile_high - vt.pctile_low);
                eff = vt.target_annualized_max +
                      t * (vt.target_annualized_min - vt.target_annualized_max);
            }
        }
        current_vol_target_ = eff;

        if (record->bpgv_garch > 0.0) {
            current_garch_sigma_natural_ =
                record->bpgv_garch * std::sqrt(12.0) * vt.garch_natural_vol_multiplier;
        } else {
            current_garch_sigma_natural_ = 0.0;
        }
        current_bpgv_garch_percentile_ = record->bpgv_garch_percentile;
    }

    // Base split, then the defensive sleeve (risk-off pair only, so its splice survives the
    // risk-on overlays), the risk-on overlays, breadth, normalisation to 1, and finally the
    // volatility target and market-stress throttle, which leave the residual in cash.
    auto weights = calculate_base_weights(*record);
    apply_defensive_sleeve_vol_awareness(weights, *record);
    apply_momentum_tilt(weights);
    apply_homebuilder_tilt(weights, *record);
    apply_housing_exposure_cap(weights, *record);
    apply_duration_defense_guard(weights);
    apply_breakout_filter(weights);
    apply_breadth_risk_permission(weights);
    normalize_weights(weights);
    pre_vol_target_weights_ = weights;
    apply_portfolio_vol_target(weights);
    pre_market_stress_weights_ = weights;
    market_stress_active_ = apply_market_stress_throttle(weights);

    current_weights_ = weights;
    update_positions_from_weights();

    last_rebalance_year_ = year;
    last_rebalance_month_ = month;
    ++rebalance_count_;

    DEBUG("Rebalanced " + std::to_string(year) + "-" + std::to_string(month) +
          " | regime_score=" + std::to_string(record->regime_score) +
          " | strong_risk_on=" + std::to_string(record->strong_risk_on));
}

// ============================================================================
// Weight pipeline
// ============================================================================

std::unordered_map<std::string, double> BPGVRotationStrategy::calculate_base_weights(
    const MonthlyMacroRecord& rec) const {

    // regime_score in [-1, +1] maps onto [base_risk_off_min, + base_risk_off_range].
    double risk_off_pct = bpgv_config_.base_risk_off_min +
                          bpgv_config_.base_risk_off_range * ((rec.regime_score + 1.0) / 2.0);

    if (bpgv_config_.bpg_growth_signal.enabled) {
        double growth_signal = compute_bpg_growth_signal(rec);
        risk_off_pct -= growth_signal * bpgv_config_.bpg_growth_signal.max_risk_off_shift;
    }

    double risk_off_min = bpgv_config_.base_risk_off_min;
    double risk_off_max = bpgv_config_.base_risk_off_min + bpgv_config_.base_risk_off_range;
    risk_off_pct = std::max(risk_off_min, std::min(risk_off_max, risk_off_pct));
    double risk_on_pct = 1.0 - risk_off_pct;

    std::unordered_map<std::string, double> weights;

    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());

    double n_risk_on = static_cast<double>(bpgv_config_.risk_on_symbols.size());
    double n_risk_off = static_cast<double>(bpgv_config_.risk_off_symbols.size());

    // Equal weight within each sleeve: the risk-on extreme puts 95% in risk-on names, the
    // risk-off extreme 70% in risk-off names.
    for (const auto& sym : all_symbols) {
        bool is_risk_on = std::find(bpgv_config_.risk_on_symbols.begin(),
                                    bpgv_config_.risk_on_symbols.end(), sym)
                          != bpgv_config_.risk_on_symbols.end();
        double on_w = is_risk_on ? (0.95 / n_risk_on) : (0.05 / n_risk_off);
        double off_w = is_risk_on ? (0.30 / n_risk_on) : (0.70 / n_risk_off);
        weights[sym] = on_w * risk_on_pct + off_w * risk_off_pct;
    }

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

double BPGVRotationStrategy::compute_bpg_growth_signal(const MonthlyMacroRecord& rec) const {
    const auto& cfg = bpgv_config_.bpg_growth_signal;
    if (!cfg.enabled || macro_data_.empty()) return 0.0;

    int idx = -1;
    for (int i = static_cast<int>(macro_data_.size()) - 1; i >= 0; --i) {
        const auto& r = macro_data_[i];
        if (r.year == rec.year && r.month == rec.month) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return 0.0;

    double sum = 0.0;
    int count = 0;
    for (int i = idx; i >= 0 && count < cfg.lookback_months; --i) {
        const auto& r = macro_data_[i];
        double g = (cfg.use_sfh_bpg && std::abs(r.bpg_sfh) > 1e-12)
            ? r.bpg_sfh
            : r.permit_growth;
        if (!std::isfinite(g)) continue;
        sum += g;
        ++count;
    }
    if (count < cfg.min_history_months) return 0.0;

    double avg = sum / static_cast<double>(count);
    double signal = avg / cfg.growth_scale;
    return std::max(-1.0, std::min(1.0, signal));
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
    return std::sqrt(var) * std::sqrt(252.0);  // annualised
}

double BPGVRotationStrategy::compute_weighted_bucket_vol(
    const std::vector<std::string>& symbols,
    const std::unordered_map<std::string, double>& weights,
    int window) const {

    if (symbols.empty() || window <= 1) return 0.0;

    std::unordered_map<std::string, double> local_weights;
    double total = 0.0;
    for (const auto& sym : symbols) {
        auto state_it = symbol_state_.find(sym);
        if (state_it == symbol_state_.end()) return 0.0;
        if (static_cast<int>(state_it->second.price_history.size()) < window + 1) return 0.0;

        double w = 0.0;
        auto w_it = weights.find(sym);
        if (w_it != weights.end()) w = std::max(0.0, w_it->second);
        local_weights[sym] = w;
        total += w;
    }

    if (total <= 1e-12) {  // an empty sleeve is measured equal-weighted
        total = static_cast<double>(symbols.size());
        for (const auto& sym : symbols) local_weights[sym] = 1.0;
    }

    std::vector<double> bucket_returns;
    bucket_returns.reserve(window);
    for (int offset = window; offset >= 1; --offset) {
        double r = 0.0;
        for (const auto& sym : symbols) {
            const auto& prices = symbol_state_.at(sym).price_history;
            int n = static_cast<int>(prices.size());
            double prev = prices[n - offset - 1];
            double cur = prices[n - offset];
            if (prev < 1e-8) continue;
            r += (local_weights[sym] / total) * ((cur - prev) / prev);
        }
        bucket_returns.push_back(r);
    }

    if (bucket_returns.size() < 2) return 0.0;
    double mean = 0.0;
    for (double r : bucket_returns) mean += r;
    mean /= static_cast<double>(bucket_returns.size());
    double var = 0.0;
    for (double r : bucket_returns) var += (r - mean) * (r - mean);
    var /= static_cast<double>(bucket_returns.size() - 1);
    return std::sqrt(var) * std::sqrt(252.0);
}

void BPGVRotationStrategy::apply_momentum_tilt(
    std::unordered_map<std::string, double>& weights) const {

    const auto& mcfg = bpgv_config_.momentum;

    // Time-series gate: zero a risk-on name whose 12-month return trails the risk-free
    // symbol's by more than tsm_tolerance. Short history passes when fail-open.
    if (mcfg.tsm_gate_enabled) {
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
                    weights[sym] = 0.0;
                }
                continue;
            }

            double r_12m = calculate_trailing_return(it->second.price_history,
                                                    mcfg.tsm_lookback_days);
            double excess = r_12m - rf_excess;
            if (excess <= -mcfg.tsm_tolerance) {
                weights[sym] = 0.0;
            }
        }
    }

    // Cross-sectional tilt: rank the surviving names by return / vol and scale by
    // 1 + tau x rank_score, rank_score running from +1 (best) to -1 (worst).
    struct MomentumEntry {
        std::string symbol;
        double score;
    };
    std::vector<MomentumEntry> entries;

    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto wit = weights.find(sym);
        if (wit == weights.end() || wit->second <= 1e-8) continue;

        auto it = symbol_state_.find(sym);
        if (it == symbol_state_.end()) continue;

        if (static_cast<int>(it->second.price_history.size()) <= mcfg.xsec_lookback_days) {
            continue;
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
        double rank_score = (n > 1) ? (1.0 - 2.0 * i / (n - 1)) : 0.0;
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

void BPGVRotationStrategy::apply_housing_exposure_cap(
    std::unordered_map<std::string, double>& weights,
    const MonthlyMacroRecord& rec) const {

    const auto& cfg = bpgv_config_.housing_exposure;
    if (!cfg.enabled) return;
    if (rec.bpgv_garch <= 0.0) return;
    if (rec.bpgv_garch_percentile < cfg.garch_percentile_trigger) return;

    double growth_signal = compute_bpg_growth_signal(rec);
    if (growth_signal > cfg.growth_signal_trigger) return;

    double released = 0.0;
    for (const auto& sym : cfg.symbols) {
        auto it = weights.find(sym);
        if (it == weights.end() || it->second <= 0.0) continue;
        double old_w = it->second;
        it->second *= cfg.cap_scale;
        released += old_w - it->second;
    }

    if (released > 1e-10) {
        weights[cfg.splice_symbol] += released;
        DEBUG("housing_exposure_cap: bpgv_garch_pct=" +
              std::to_string(rec.bpgv_garch_percentile) +
              " growth_signal=" + std::to_string(growth_signal) +
              " released=" + std::to_string(released) +
              " splice=" + cfg.splice_symbol);
    }
}

void BPGVRotationStrategy::apply_duration_defense_guard(
    std::unordered_map<std::string, double>& weights) const {

    const auto& cfg = bpgv_config_.duration_defense;
    if (!cfg.enabled) return;

    auto wit = weights.find(cfg.duration_symbol);
    if (wit == weights.end() || wit->second <= 0.0) return;

    auto sit = symbol_state_.find(cfg.duration_symbol);
    if (sit == symbol_state_.end()) return;
    const auto& prices = sit->second.price_history;

    int needed = std::max(cfg.trend_lookback_days + 1, cfg.sma_window);
    if (static_cast<int>(prices.size()) < needed) return;

    bool poor = true;
    if (cfg.require_negative_return) {
        double trailing = calculate_trailing_return(prices, cfg.trend_lookback_days);
        poor = poor && (trailing < 0.0);
    }
    if (cfg.require_below_sma) {
        double sma = calculate_sma(prices, cfg.sma_window);
        poor = poor && (prices.back() < sma);
    }
    if (!poor) return;

    double old_w = wit->second;
    wit->second *= cfg.guard_scale;
    double released = old_w - wit->second;
    if (released > 1e-10) {
        weights[cfg.splice_symbol] += released;
        DEBUG("duration_defense_guard: cut " + cfg.duration_symbol +
              " by scale=" + std::to_string(cfg.guard_scale) +
              " released=" + std::to_string(released) +
              " splice=" + cfg.splice_symbol);
    }
}

double BPGVRotationStrategy::compute_breadth_score() const {
    const auto& cfg = bpgv_config_.breadth_risk;
    current_breadth_valid_ = false;
    current_breadth_score_ = 1.0;

    if (!cfg.enabled) return 1.0;

    std::vector<std::string> symbols = cfg.symbols.empty()
        ? bpgv_config_.risk_on_symbols
        : cfg.symbols;
    if (symbols.empty()) return 1.0;

    double rf_return = 0.0;
    bool have_rf = false;
    auto rf_it = symbol_state_.find(cfg.risk_free_symbol);
    if (rf_it != symbol_state_.end() &&
        static_cast<int>(rf_it->second.price_history.size()) >= cfg.momentum_lookback_days + 1) {
        rf_return = calculate_trailing_return(
            rf_it->second.price_history, cfg.momentum_lookback_days);
        have_rf = true;
    }

    int valid = 0;
    int pass = 0;
    const int needed = std::max({cfg.min_history_days,
                                 cfg.momentum_lookback_days + 1,
                                 cfg.sma_window});

    for (const auto& sym : symbols) {
        auto it = symbol_state_.find(sym);
        if (it == symbol_state_.end() ||
            static_cast<int>(it->second.price_history.size()) < needed) {
            // Too little history: counted as passing when fail-open, else left out.
            if (cfg.fail_open_on_short_history) {
                ++valid;
                ++pass;
            }
            continue;
        }

        const auto& prices = it->second.price_history;
        bool ok = true;

        if (cfg.require_positive_excess) {
            double excess = calculate_trailing_return(prices, cfg.momentum_lookback_days)
                            - (have_rf ? rf_return : 0.0);
            ok = ok && (excess >= 0.0);
        }

        if (cfg.require_above_sma) {
            double sma = calculate_sma(prices, cfg.sma_window);
            ok = ok && (prices.back() >= sma);
        }

        ++valid;
        if (ok) ++pass;
    }

    if (valid <= 0) return 1.0;

    current_breadth_valid_ = true;
    current_breadth_score_ = static_cast<double>(pass) / static_cast<double>(valid);
    return current_breadth_score_;
}

bool BPGVRotationStrategy::is_breadth_stress_active() const {
    const auto& cfg = bpgv_config_.breadth_risk;
    if (!cfg.enabled) return false;

    double breadth = compute_breadth_score();
    return current_breadth_valid_ && breadth <= cfg.stress_breadth;
}

void BPGVRotationStrategy::apply_breadth_risk_permission(
    std::unordered_map<std::string, double>& weights) const {

    const auto& cfg = bpgv_config_.breadth_risk;
    if (!cfg.enabled) return;

    double breadth = compute_breadth_score();
    if (!current_breadth_valid_) return;
    if (breadth >= cfg.full_risk_breadth) return;

    double permission = breadth / cfg.full_risk_breadth;
    permission = std::min(1.0, permission);
    permission = std::max(cfg.risk_on_floor, permission);

    double released = 0.0;
    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto it = weights.find(sym);
        if (it == weights.end() || it->second <= 0.0) continue;
        double old_w = it->second;
        it->second *= permission;
        released += old_w - it->second;
    }

    if (released > 1e-10) {
        weights[cfg.splice_symbol] += released;
        DEBUG("breadth_risk_permission: breadth=" + std::to_string(breadth) +
              " full=" + std::to_string(cfg.full_risk_breadth) +
              " permission=" + std::to_string(permission) +
              " released=" + std::to_string(released) +
              " splice=" + cfg.splice_symbol);
    }
}

double BPGVRotationStrategy::wilder_atr(const std::deque<double>& high,
                                        const std::deque<double>& low,
                                        const std::deque<double>& close,
                                        int period) {
    int n = static_cast<int>(close.size());
    if (n <= period || static_cast<int>(high.size()) != n ||
        static_cast<int>(low.size()) != n) {
        return 0.0;
    }

    // TR_i = max(H_i - L_i, |H_i - C_{i-1}|, |L_i - C_{i-1}|)
    std::vector<double> tr;
    tr.reserve(n - 1);
    for (int i = 1; i < n; ++i) {
        double h_l = high[i] - low[i];
        double h_pc = std::abs(high[i] - close[i - 1]);
        double l_pc = std::abs(low[i] - close[i - 1]);
        tr.push_back(std::max({h_l, h_pc, l_pc}));
    }

    if (static_cast<int>(tr.size()) < period) return 0.0;
    double atr = 0.0;
    for (int i = 0; i < period; ++i) atr += tr[i];
    atr /= period;

    // ATR_i = ((period - 1) x ATR_{i-1} + TR_i) / period
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

    // confirmation_days consecutive closes below the SMA zero the name.
    if (bcfg.confirmation_days > 0 && static_cast<int>(close.size()) >= bcfg.confirmation_days) {
        int count_below = 0;
        for (int i = n - bcfg.confirmation_days; i < n; ++i) {
            if (close[i] < sma) ++count_below;
        }
        if (count_below == bcfg.confirmation_days) return 0.0;
    }

    if (!bcfg.graded_weighting) {
        return (close.back() >= sma) ? 1.0 : 0.0;
    }

    if (atr < 1e-10) {
        return (close.back() >= sma) ? 1.0 : 0.0;
    }
    double raw = (close.back() - sma) / (bcfg.atr_k * atr);
    return std::max(0.0, std::min(1.0, raw));
}

void BPGVRotationStrategy::apply_breakout_filter(
    std::unordered_map<std::string, double>& weights) const {

    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto state_it = symbol_state_.find(sym);
        if (state_it == symbol_state_.end()) continue;
        double m = breakout_score(state_it->second.high_history,
                                  state_it->second.low_history,
                                  state_it->second.price_history);
        weights[sym] *= m;
    }
}

void BPGVRotationStrategy::normalize_weights(
    std::unordered_map<std::string, double>& weights) const {

    double total = 0.0;
    for (const auto& [sym, w] : weights) {
        total += std::max(0.0, w);
    }

    if (total < 1e-8) {
        // Every filter zeroed everything: equal weight across the whole universe, cash
        // symbols included, rather than an empty book.
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

void BPGVRotationStrategy::apply_defensive_sleeve_vol_awareness(
    std::unordered_map<std::string, double>& weights,
    const MonthlyMacroRecord& rec) const {

    const auto& cfg = bpgv_config_.defensive_sleeve;
    if (!cfg.vol_aware_enabled) return;

    double effective_pair_target = cfg.pair_vol_target_annualized;
    if (cfg.garch_aware_enabled && rec.bpgv_garch > 0.0) {
        double pct = rec.bpgv_garch_percentile;
        if (pct >= cfg.garch_pctile_high) {
            effective_pair_target = cfg.pair_vol_target_high_sigma;
        } else if (pct <= cfg.garch_pctile_low) {
            effective_pair_target = cfg.pair_vol_target_low_sigma;
        } else {
            double t = (pct - cfg.garch_pctile_low)
                       / (cfg.garch_pctile_high - cfg.garch_pctile_low);
            effective_pair_target = cfg.pair_vol_target_low_sigma
                + t * (cfg.pair_vol_target_high_sigma - cfg.pair_vol_target_low_sigma);
        }
    }

    double bucket_total = 0.0;
    for (const auto& sym : bpgv_config_.risk_off_symbols) {
        auto it = weights.find(sym);
        if (it != weights.end()) bucket_total += std::max(0.0, it->second);
    }
    if (bucket_total < 1e-8) return;

    // Short history on either leg: leave the bucket as it is.
    std::unordered_map<std::string, double> sigma;
    std::unordered_map<std::string, double> inv_sigma;
    double inv_sigma_total = 0.0;
    bool short_history = false;

    for (const auto& sym : bpgv_config_.risk_off_symbols) {
        auto it = symbol_state_.find(sym);
        if (it == symbol_state_.end() ||
            static_cast<int>(it->second.price_history.size()) <
                cfg.min_history_days + 1) {
            short_history = true;
            break;
        }
        double s = compute_symbol_vol(it->second.price_history, cfg.vol_window_days);
        if (s < 1e-6) {
            short_history = true;
            break;
        }
        sigma[sym] = s;
        inv_sigma[sym] = 1.0 / s;
        inv_sigma_total += inv_sigma[sym];
    }
    if (short_history || inv_sigma_total < 1e-12) return;

    std::unordered_map<std::string, double> pair_weight;
    if (cfg.use_inverse_vol_pair) {
        for (const auto& sym : bpgv_config_.risk_off_symbols) {
            pair_weight[sym] = inv_sigma[sym] / inv_sigma_total;
        }
    } else {
        double n = static_cast<double>(bpgv_config_.risk_off_symbols.size());
        for (const auto& sym : bpgv_config_.risk_off_symbols) {
            pair_weight[sym] = 1.0 / n;
        }
    }

    // Joint realized vol of the weighted pair, so co-movement counts.
    int window = cfg.vol_window_days;
    int min_len = INT_MAX;
    for (const auto& sym : bpgv_config_.risk_off_symbols) {
        auto it = symbol_state_.find(sym);
        min_len = std::min(min_len,
                           static_cast<int>(it->second.price_history.size()));
    }
    if (min_len < window + 1) return;

    std::vector<double> pair_returns;
    pair_returns.reserve(window);
    for (int i = min_len - window; i < min_len; ++i) {
        double r = 0.0;
        for (const auto& sym : bpgv_config_.risk_off_symbols) {
            const auto& ph = symbol_state_.at(sym).price_history;
            double prev = ph[i - 1];
            double cur = ph[i];
            if (prev < 1e-8) continue;
            r += pair_weight[sym] * ((cur - prev) / prev);
        }
        pair_returns.push_back(r);
    }
    if (pair_returns.size() < 2) return;
    double mean = 0.0;
    for (double r : pair_returns) mean += r;
    mean /= pair_returns.size();
    double var = 0.0;
    for (double r : pair_returns) var += (r - mean) * (r - mean);
    var /= (pair_returns.size() - 1);
    double sigma_joint = std::sqrt(var) * std::sqrt(252.0);
    if (sigma_joint < 1e-6) return;

    double pair_scale = std::min(1.0, effective_pair_target / sigma_joint);
    pair_scale = std::max(1.0 - cfg.max_splice_fraction, pair_scale);

    double splice_amount = bucket_total * (1.0 - pair_scale);

    for (const auto& sym : bpgv_config_.risk_off_symbols) {
        weights[sym] = bucket_total * pair_scale * pair_weight[sym];
    }
    weights[cfg.splice_symbol] += splice_amount;

    DEBUG("defensive_sleeve_vol_awareness: sigma_joint=" +
          std::to_string(sigma_joint) + " effective_pair_target=" +
          std::to_string(effective_pair_target) +
          " (garch_aware=" + std::to_string(cfg.garch_aware_enabled) +
          ", bpgv_garch_pct=" + std::to_string(rec.bpgv_garch_percentile) +
          ") pair_scale=" + std::to_string(pair_scale) + " splice->" + cfg.splice_symbol +
          " bucket=" + std::to_string(bucket_total) +
          " splice=" + std::to_string(splice_amount));
}

void BPGVRotationStrategy::apply_portfolio_vol_target(
    std::unordered_map<std::string, double>& weights) const {

    const auto& cfg = bpgv_config_.vol_target;
    if (!cfg.enabled) return;

    double sigma_annual = 0.0;
    std::string sigma_source;
    bool use_max_risk_sigma = cfg.use_max_risk_sigma;
    if (cfg.use_max_risk_sigma && cfg.conditional_max_risk_sigma) {
        use_max_risk_sigma = is_confirmed_risk_stress_active();
    }

    // Max-risk: the largest of the available sigmas, so adding a risk source can only cut
    // gross. Otherwise the month's GARCH forecast, else the book's realized volatility.
    if (use_max_risk_sigma) {
        std::vector<std::pair<std::string, double>> candidates;

        if (cfg.include_garch_sigma && current_garch_sigma_natural_ > 0.0) {
            candidates.push_back({"bpgv_garch", current_garch_sigma_natural_});
        }

        if (cfg.include_realized_portfolio_sigma) {
            int n = static_cast<int>(portfolio_value_history_.size());
            if (n >= cfg.window_days + 1 && n >= cfg.min_history_days + 1) {
                double s = compute_daily_return_stdev(cfg.window_days) * std::sqrt(252.0);
                if (s > 1e-8) candidates.push_back({"portfolio_realized", s});
            }
        }

        if (cfg.include_stress_symbol_sigma) {
            auto it = symbol_state_.find(cfg.stress_vol_symbol);
            if (it != symbol_state_.end()) {
                double s = compute_symbol_vol(it->second.price_history,
                                              cfg.stress_vol_window_days);
                if (s > 1e-8) candidates.push_back({"stress_symbol", s});
            }
        }

        if (cfg.include_defensive_pair_sigma) {
            double s = compute_weighted_bucket_vol(
                bpgv_config_.risk_off_symbols, weights,
                cfg.defensive_pair_vol_window_days);
            if (s > 1e-8) candidates.push_back({"defensive_pair", s});
        }

        if (candidates.empty()) return;
        auto max_it = std::max_element(
            candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        sigma_annual = max_it->second;
        sigma_source = "max-risk:" + max_it->first;
    } else if (cfg.use_garch_natural_vol && current_garch_sigma_natural_ > 0.0) {
        sigma_annual = current_garch_sigma_natural_;
        sigma_source = "bpgv_garch";
    } else {
        // Realized volatility of the book; too little history leaves the weights unscaled.
        int n = static_cast<int>(portfolio_value_history_.size());
        if (n < cfg.min_history_days + 1) return;
        if (n < cfg.window_days + 1) return;
        double sigma_daily = compute_daily_return_stdev(cfg.window_days);
        if (sigma_daily < 1e-8) return;
        sigma_annual = sigma_daily * std::sqrt(252.0);
        sigma_source = "realized";
    }

    double effective_target = (cfg.enable_dynamic_target && current_vol_target_ > 0.0)
        ? current_vol_target_ : cfg.target_annualized;

    double scalar = effective_target / sigma_annual;
    scalar = std::min(1.0, scalar);
    scalar = std::max(cfg.scalar_floor, scalar);

    for (auto& [sym, w] : weights) {
        w *= scalar;
    }

    DEBUG("portfolio_vol_target [" + sigma_source +
          "]: sigma_annual=" + std::to_string(sigma_annual) +
          " target=" + std::to_string(effective_target) +
          " (dynamic=" + std::to_string(cfg.enable_dynamic_target) +
          ", conditional_max_risk=" + std::to_string(cfg.conditional_max_risk_sigma) +
          ") scalar=" + std::to_string(scalar));
}

bool BPGVRotationStrategy::is_confirmed_risk_stress_active() const {
    const auto& cfg = bpgv_config_.vol_target;
    if (!cfg.use_max_risk_sigma) return false;
    if (!cfg.conditional_max_risk_sigma) return true;

    bool active = false;
    if (cfg.max_risk_on_market_stress && is_market_stress_active()) {
        active = true;
    }
    if (cfg.max_risk_on_breadth_stress && is_breadth_stress_active()) {
        active = true;
    }
    return active;
}

bool BPGVRotationStrategy::is_market_stress_active() const {
    const auto& cfg = bpgv_config_.market_stress;
    if (!cfg.enabled) return false;

    auto it = symbol_state_.find(cfg.stress_symbol);
    if (it == symbol_state_.end()) return false;

    const auto& prices = it->second.price_history;
    const int needed = std::max({cfg.min_history_days,
                                 cfg.sma_window,
                                 cfg.drawdown_lookback_days + 1});
    if (static_cast<int>(prices.size()) < needed) return false;

    double trailing = calculate_trailing_return(prices, cfg.drawdown_lookback_days);
    double sma = calculate_sma(prices, cfg.sma_window);
    double spot = prices.back();

    return trailing <= cfg.drawdown_trigger && spot < sma;
}

bool BPGVRotationStrategy::apply_market_stress_throttle(
    std::unordered_map<std::string, double>& weights) const {

    if (!is_market_stress_active()) return false;

    const auto& cfg = bpgv_config_.market_stress;
    for (const auto& sym : bpgv_config_.risk_on_symbols) {
        auto it = weights.find(sym);
        if (it != weights.end()) {
            it->second *= cfg.risk_on_scale;
        }
    }

    DEBUG("market_stress_throttle active: " + cfg.stress_symbol +
          " trailing_" + std::to_string(cfg.drawdown_lookback_days) +
          "d <= " + std::to_string(cfg.drawdown_trigger) +
          " and below SMA" + std::to_string(cfg.sma_window) +
          "; risk_on_scale=" + std::to_string(cfg.risk_on_scale));

    return true;
}

void BPGVRotationStrategy::refresh_market_stress_overlay() {
    if (pre_market_stress_weights_.empty() && pre_vol_target_weights_.empty()) return;

    auto adjusted = pre_vol_target_weights_.empty()
        ? pre_market_stress_weights_
        : pre_vol_target_weights_;

    // Re-run the volatility target first, so max-risk sigma can engage when stress breaks
    // mid-month, then the market-stress haircut.
    if (!pre_vol_target_weights_.empty()) {
        apply_portfolio_vol_target(adjusted);
        pre_market_stress_weights_ = adjusted;
    }

    bool active = apply_market_stress_throttle(adjusted);

    bool changed = (active != market_stress_active_);
    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.cash_symbols.begin(),
                       bpgv_config_.cash_symbols.end());

    for (const auto& sym : all_symbols) {
        double old_w = 0.0;
        auto old_it = current_weights_.find(sym);
        if (old_it != current_weights_.end()) old_w = old_it->second;

        double new_w = 0.0;
        auto new_it = adjusted.find(sym);
        if (new_it != adjusted.end()) new_w = new_it->second;

        if (std::abs(old_w - new_w) > 1e-8) {
            changed = true;
            break;
        }
    }

    if (!changed) return;

    current_weights_ = adjusted;
    market_stress_active_ = active;
    update_positions_from_weights();

    INFO(std::string("BPGV daily risk overlay refreshed: market_stress=") +
         (active ? "active" : "inactive") +
         ", confirmed_risk_stress=" +
         (is_confirmed_risk_stress_active() ? "active" : "inactive"));
}

double BPGVRotationStrategy::compute_daily_return_stdev(int window) const {
    // Sample stdev of the daily returns of portfolio_value_history_ over the last window days.
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

// ============================================================================
// Position management
// ============================================================================

double exposure_increment_scale(const std::vector<ExposureLeg>& legs, double nav) {
    if (!std::isfinite(nav) || nav <= 0.0) return 0.0;
    double kept = 0.0;        // targets of the legs that add nothing, holdings of those that do
    double increments = 0.0;  // what the adding legs want on top of their holdings
    for (const auto& leg : legs) {
        if (!std::isfinite(leg.held_notional) || !std::isfinite(leg.target_notional)) return 0.0;
        if (leg.target_notional > leg.held_notional) {
            kept += leg.held_notional;
            increments += leg.target_notional - leg.held_notional;
        } else {
            kept += leg.target_notional;
        }
    }
    if (increments <= 0.0) return 1.0;
    const double room = nav - kept;
    if (room <= 0.0) return 0.0;
    return std::min(1.0, room / increments);
}

// Holdings at current prices, without cash; 0 when the book is flat.
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
    // Targets are materialised in get_target_positions(); this only reports a weighted
    // symbol that has no price to size it with.
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

    // Size to the prior-close NAV so the book compounds; before the first push, to the
    // configured allocation.
    const double sizing_capital =
        (sizing_equity_ > 1e-8) ? sizing_equity_
                                : static_cast<double>(config_.capital_allocation);

    // A row for every universe symbol, so a dropped name shows as qty 0 and is closed.
    auto all_symbols = bpgv_config_.risk_on_symbols;
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.risk_off_symbols.begin(),
                       bpgv_config_.risk_off_symbols.end());
    all_symbols.insert(all_symbols.end(),
                       bpgv_config_.cash_symbols.begin(),
                       bpgv_config_.cash_symbols.end());

    // Tolerance band (RebalanceConfig): trade only when the realised weight drifts outside the
    // band, and then halfway back; full exits and full entries stay decisive.
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

        double current_qty = 0.0;
        auto pos_it = positions_.find(sym);
        if (pos_it != positions_.end()) {
            current_qty = pos_it->second.quantity.as_double();
        }
        double current_notional = current_qty * price;
        double w_current = (sizing_capital > 1e-8) ? (current_notional / sizing_capital) : 0.0;

        double w_effective = w_current;
        bool trade = false;

        const bool want_zero = (w_target <= 1e-8);
        const bool have_none = (std::abs(w_current) <= 1e-8);

        if (want_zero && !have_none && rcfg.exit_fully_on_zero_target) {
            w_effective = 0.0;
            trade = true;
        } else if (!want_zero && have_none && rcfg.enter_fully_on_zero_current) {
            w_effective = w_target;
            trade = true;
        } else if (!want_zero) {
            double drift_abs = w_current - w_target;
            double drift_rel = drift_abs / w_target;
            bool trigger = (std::abs(drift_abs) > rcfg.drift_abs_trigger) ||
                           (std::abs(drift_rel) > rcfg.drift_rel_trigger);
            if (trigger) {
                w_effective = rcfg.halfway_rule
                                  ? (w_current - 0.5 * drift_abs)
                                  : w_target;
                trade = true;
            }
        }

        // Inside the band the target IS the held quantity, copied as-is: re-deriving it as
        // w_current x C / p differs from the held Decimal by floating-point noise, and
        // PortfolioManager trades any |target - filled| > 1e-6.
        if (!trade) {
            pos.quantity = (pos_it != positions_.end()) ? pos_it->second.quantity : Quantity(0.0);
        } else if (w_effective > 1e-8 && price > 1e-8) {
            double target_shares = (w_effective * sizing_capital) / price;
            target_shares = bpgv_config_.allow_fractional_shares
                                ? std::round(target_shares * 1e6) / 1e6
                                : std::floor(target_shares);
            pos.quantity = Quantity(target_shares);
        } else if (w_effective <= 1e-8) {
            pos.quantity = Quantity(0.0);
        }
        // A weighted symbol without a price stays at 0 rather than sizing on a stale price.

        if (pos_it != positions_.end()) {
            pos.average_price = pos_it->second.average_price;
            pos.realized_pnl = pos_it->second.realized_pnl;
            pos.unrealized_pnl = pos_it->second.unrealized_pnl;
            pos.last_update = pos_it->second.last_update;
        }

        targets[sym] = pos;
    }

    // Cash cap. The band trims a held name only halfway while a new name enters in full, so
    // one rebalance can ask for more than NAV (held {A .5, B .4} -> targets {A .3, B .25,
    // C .4, D .4} asks for 1.35x), which a long-only cash book cannot fund. Only the legs that
    // ADD exposure are scaled, on their increment over the holding, so a trimmed, exited or
    // in-band leg keeps its target exactly. Scaled legs round DOWN so the book stays <= NAV.
    {
        std::vector<ExposureLeg> legs;
        std::vector<std::pair<std::string, double>> adding;  // symbol, price
        legs.reserve(targets.size());
        for (const auto& [sym, pos] : targets) {
            auto s_it = symbol_state_.find(sym);
            const double px = (s_it != symbol_state_.end()) ? s_it->second.current_price : 0.0;
            if (px <= 1e-8) continue;  // unpriced: its target is 0 above, nothing to fund
            auto held_it = positions_.find(sym);
            const double held_qty =
                (held_it != positions_.end()) ? held_it->second.quantity.as_double() : 0.0;
            const ExposureLeg leg{held_qty * px, pos.quantity.as_double() * px};
            legs.push_back(leg);
            if (leg.target_notional > leg.held_notional) adding.emplace_back(sym, px);
        }
        const double scale = exposure_increment_scale(legs, sizing_capital);
        if (scale < 1.0 && !adding.empty()) {
            double asked = 0.0;
            for (const auto& leg : legs) asked += leg.target_notional;
            for (const auto& [sym, px] : adding) {
                auto& pos = targets[sym];
                auto held_it = positions_.find(sym);
                const double held_qty =
                    (held_it != positions_.end()) ? held_it->second.quantity.as_double() : 0.0;
                const double capped_qty =
                    held_qty + scale * (pos.quantity.as_double() - held_qty);
                const double rounded = bpgv_config_.allow_fractional_shares
                                           ? std::floor(capped_qty * 1e6) / 1e6
                                           : std::floor(capped_qty);
                if (held_it != positions_.end() && rounded <= held_qty + 1e-6) {
                    // Nothing left to add: the held Decimal as-is, as the band rule copies an
                    // in-band holding.
                    pos.quantity = held_it->second.quantity;
                } else {
                    pos.quantity = Quantity(rounded);
                }
            }
            INFO("BPGV_NAV_CAP nav=" + std::to_string(sizing_capital) +
                 " asked=" + std::to_string(asked) + " scale=" + std::to_string(scale) +
                 " adding_legs=" + std::to_string(adding.size()) +
                 ": the band rule asked for more than NAV; the adding legs are funded from cash");
        }
    }

    return targets;
}

// ============================================================================
// Helpers
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
    // UTC, never the host zone: bars are UTC-midnight on every path, and localtime on a
    // New York host reported the previous calendar day, shifting the day-of-month gate.
    auto tt = std::chrono::system_clock::to_time_t(ts);
    std::tm tm{};
    core::safe_gmtime(&tt, &tm);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

}  // namespace trade_ngin
