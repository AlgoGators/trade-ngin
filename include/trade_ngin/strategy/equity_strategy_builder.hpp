#pragma once

// Shared helpers for the equity portfolio runners (bt_equity_mean_reversion,
// live_equity_mean_reversion). Centralizes (a) parsing a MeanReversionConfig from
// a strategy's JSON "config" block so the backtest and live runners build the
// strategy identically (review T-OR.5), and (b) iterating a portfolio's
// strategies_config to collect the *enabled* strategies while ERRORing on an
// unknown strategy type instead of silently dropping it (review §F10).

#include <algorithm>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"
#include "trade_ngin/strategy/bpgv_rotation.hpp"
#include "trade_ngin/strategy/mean_reversion.hpp"

namespace trade_ngin {
namespace apps {

// Strategy type strings recognized by the equity runners. Extend this (and the
// per-runner construction dispatch) when a further equity strategy is authored.
inline const std::vector<std::string>& known_equity_strategy_types() {
    static const std::vector<std::string> kTypes = {"MeanReversionStrategy",
                                                    "BPGVRotationStrategy"};
    return kTypes;
}

// "A, B" rendering of known_equity_strategy_types() for error messages, so the list
// in the text can never drift from the list in the code.
inline std::string join_known_equity_strategy_types() {
    std::string out;
    for (const auto& t : known_equity_strategy_types()) out += (out.empty() ? "" : ", ") + t;
    return out;
}

// Parse a MeanReversionConfig from a strategy's "config" JSON block. Any absent
// key falls back to the MeanReversionConfig struct default, so this is a strict
// superset of what either runner parsed inline previously.
inline MeanReversionConfig build_mean_reversion_config(const nlohmann::json& cfg) {
    MeanReversionConfig mr;  // start from struct defaults
    mr.lookback_period         = cfg.value("lookback_period", mr.lookback_period);
    mr.entry_threshold         = cfg.value("entry_threshold", mr.entry_threshold);
    mr.exit_threshold          = cfg.value("exit_threshold", mr.exit_threshold);
    mr.risk_target             = cfg.value("risk_target", mr.risk_target);
    mr.position_size           = cfg.value("position_size", mr.position_size);
    mr.vol_lookback            = cfg.value("vol_lookback", mr.vol_lookback);
    mr.use_stop_loss           = cfg.value("use_stop_loss", mr.use_stop_loss);
    mr.stop_loss_pct           = cfg.value("stop_loss_pct", mr.stop_loss_pct);
    mr.allow_fractional_shares = cfg.value("allow_fractional_shares", mr.allow_fractional_shares);
    mr.fractional_min_price    = cfg.value("fractional_min_price", mr.fractional_min_price);
    mr.fractional_min_adv      = cfg.value("fractional_min_adv", mr.fractional_min_adv);
    return mr;
}

// Parse a BPGVRotationConfig from a strategy's "config" JSON block. Delegates to
// BPGVRotationConfig::from_json so the backtest runner, the unit tests and any live
// runner construct the strategy from one parser (review T-OR.5).
inline BPGVRotationConfig build_bpgv_rotation_config(const nlohmann::json& cfg) {
    return BPGVRotationConfig::from_json(cfg);
}

// One enabled strategy definition resolved from a portfolio's strategies_config.
struct EquityStrategyEntry {
    std::string id;       // the strategies_config key, e.g. "MEAN_REVERSION"
    std::string type;     // e.g. "MeanReversionStrategy"
    double allocation;    // "default_allocation" (defaults to 1.0)
    nlohmann::json def;   // the full strategy definition
};

// Iterate strategies_config, collect the strategies enabled for the given mode,
// and validate that every enabled strategy has a recognized "type" and a "config"
// block. ERRORs (rather than silently dropping) on an unknown type or a missing
// config -- closes the silent-skip footgun in review §F10. `enabled_key` is
// "enabled_backtest" or "enabled_live".
inline Result<std::vector<EquityStrategyEntry>> collect_enabled_equity_strategies(
    const nlohmann::json& strategies_config, const std::string& enabled_key) {
    if (!strategies_config.is_object()) {
        return make_error<std::vector<EquityStrategyEntry>>(
            ErrorCode::INVALID_ARGUMENT,
            "strategies_config is not a JSON object", "equity_strategy_builder");
    }

    std::vector<EquityStrategyEntry> entries;
    for (const auto& item : strategies_config.items()) {
        const std::string& id = item.key();
        const auto& def = item.value();
        if (!def.value(enabled_key, false)) {
            continue;  // not enabled for this mode
        }

        const std::string type = def.value("type", std::string{});
        const auto& known = known_equity_strategy_types();
        if (std::find(known.begin(), known.end(), type) == known.end()) {
            return make_error<std::vector<EquityStrategyEntry>>(
                ErrorCode::INVALID_ARGUMENT,
                "Unknown equity strategy type '" + type + "' for strategy '" + id +
                    "' (known types: " + join_known_equity_strategy_types() +
                    "). Refusing to silently skip it.",
                "equity_strategy_builder");
        }
        if (!def.contains("config")) {
            return make_error<std::vector<EquityStrategyEntry>>(
                ErrorCode::INVALID_ARGUMENT,
                "Equity strategy '" + id + "' is missing its 'config' section",
                "equity_strategy_builder");
        }

        entries.push_back({id, type, def.value("default_allocation", 1.0), def});
    }

    if (entries.empty()) {
        return make_error<std::vector<EquityStrategyEntry>>(
            ErrorCode::INVALID_ARGUMENT,
            "No equity strategies enabled for '" + enabled_key + "'",
            "equity_strategy_builder");
    }
    return Result<std::vector<EquityStrategyEntry>>(std::move(entries));
}

/**
 * @brief Refuse to start when the config asks for the optimizer on an equity book.
 *
 * Both equity runners hard-code PortfolioConfig::use_optimization to false (HD
 * 2026-09-01: the dynamic optimizer is a trend-following construct that cannot even see
 * an equity strategy). Schema 2 makes `use_optimization` an explicit, required key in
 * portfolio.json, which creates a contradiction the hard-code would win silently -- the
 * operator would read `true` in the file and believe it. So the runner stops instead.
 *
 * Shared rather than written twice so the live and backtest runners cannot drift.
 *
 * @return an error carrying the refusal message, or OK (and silence) when false.
 */
inline Result<void> refuse_if_optimizer_requested(bool config_use_optimization) {
    if (!config_use_optimization) return Result<void>();
    return make_error<void>(
        ErrorCode::INVALID_DATA,
        "Refusing to start: portfolio.json sets use_optimization=true, but the equity runners "
        "do not run the optimizer (HD 2026-09-01; see the comment at the hard-code).",
        "equity_strategy_builder");
}

/**
 * @brief Refuse to start a book that sizes on its prior-close NAV while a Carver module gates it.
 *
 * A NAV-sized equity book (StrategyInterface::on_portfolio_value; BPGV) compounds its targets,
 * but nothing on the equity paths moves a risk module's capital with it: the futures book keeps
 * the two together through PortfolioManager::set_sizing_capital, the equity backtest and live
 * runner do not. Of the schema-2 modules only `carver` reads a capital base (its leverage term
 * divides by RiskConfig::capital); `none`, `constant_scale`, `warn` and `refuse` read the
 * rebalance phase only. So a Carver module on such a book would gate on leverage against the
 * configured capital while the book trades on NAV -- refused here, whether it came from the
 * files or from a desk overlay, rather than run half-wired. BPGV's ruled assignment is `none`
 * (config_template/portfolios/bpgv_rotation/risk.json).
 *
 * @return an error carrying the refusal message, or OK when no portfolio or sleeve module is a
 *         Carver gate.
 */
inline Result<void> refuse_if_nav_sized_book_has_a_capital_gate(const RiskSchema& schema) {
    auto is_carver = [](const RiskModuleConfig& m) { return m.type == "carver"; };
    bool gated = std::any_of(schema.portfolio.begin(), schema.portfolio.end(), is_carver);
    for (const auto& [sleeve, modules] : schema.sleeves) {
        gated = gated || std::any_of(modules.begin(), modules.end(), is_carver);
    }
    if (!gated) return Result<void>();
    return make_error<void>(
        ErrorCode::INVALID_DATA,
        "Refusing to start: this book sizes on its prior-close NAV, but its risk config assigns a "
        "`carver` module, whose leverage gate would divide by the configured capital while the "
        "book compounds. Assign `none` (the ruled assignment; see risk.json's _reason) or wire "
        "the module's capital to NAV (PortfolioManager::set_sizing_capital) first.",
        "equity_strategy_builder");
}

}  // namespace apps
}  // namespace trade_ngin
