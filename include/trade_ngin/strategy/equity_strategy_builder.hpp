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
#include "trade_ngin/strategy/mean_reversion.hpp"

namespace trade_ngin {
namespace apps {

// Strategy type strings recognized by the equity runners. Extend this (and the
// per-runner construction dispatch) when a second equity strategy is authored.
inline const std::vector<std::string>& known_equity_strategy_types() {
    static const std::vector<std::string> kTypes = {"MeanReversionStrategy"};
    return kTypes;
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
                    "' (known types: MeanReversionStrategy). Refusing to silently skip it.",
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

}  // namespace apps
}  // namespace trade_ngin
