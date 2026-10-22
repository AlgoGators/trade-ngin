#pragma once

// LOOP_SPEC section 7.7: a futures sleeve's risk_target, idm and vol_lookback_short are read from
// its "config" object in portfolio.json and are REQUIRED. The first sleeve's risk_target is the
// tau the risk overlay's three limits are ratios to (section 4), so a runner's own fallback value
// would move the limits without a word; the runners used to carry different fallbacks (0.15 in
// one backtest, 0.2 live). A missing or mistyped key refuses the run at load and names the key.

#include <string>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/config_loader.hpp"
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

namespace trade_ngin {

inline Result<void> read_required_sleeve_keys(const std::string& sleeve_id,
                                              const nlohmann::json& strategy_def,
                                              TrendFollowingConfig& out) {
    auto refuse = [&](const std::string& key, const std::string& what) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
                                "portfolio.json strategies." + sleeve_id + ".config." + key + " " +
                                    what +
                                    ": a futures sleeve's risk_target, idm and vol_lookback_short "
                                    "are required and have no default",
                                "SleeveConfig");
    };
    const nlohmann::json empty = nlohmann::json::object();
    const nlohmann::json& cfg =
        strategy_def.is_object() && strategy_def.contains("config") && strategy_def["config"].is_object()
            ? strategy_def["config"]
            : empty;
    for (const char* key : {"risk_target", "idm", "vol_lookback_short"}) {
        if (!cfg.contains(key)) return refuse(key, "is missing");
    }
    if (!cfg["risk_target"].is_number() || !(cfg["risk_target"].get<double>() > 0.0)) {
        return refuse("risk_target", "must be a positive number");
    }
    if (!cfg["idm"].is_number() || !(cfg["idm"].get<double>() > 0.0)) {
        return refuse("idm", "must be a positive number");
    }
    if (!cfg["vol_lookback_short"].is_number_integer() || cfg["vol_lookback_short"].get<int>() <= 0) {
        return refuse("vol_lookback_short", "must be a positive whole number");
    }
    out.risk_target = cfg["risk_target"].get<double>();
    out.idm = cfg["idm"].get<double>();
    out.vol_lookback_short = cfg["vol_lookback_short"].get<int>();
    return Result<void>();
}

/// The hand-over of portfolio.json's trading_rule_removals from the loaded application config to
/// the trend sleeve's config. It is the ONE place the list crosses from the loader to a sleeve:
/// every futures runner, backtest and live, calls it where it builds its TrendFollowingStrategy
/// sleeve, and a runner that does not would parse the block and run every contract on every pair.
/// An absent block is an empty map and changes no value.
inline void hand_over_trading_rule_removals(const AppConfig& app_config, TrendFollowingConfig& out) {
    out.rule_removals = app_config.trading_rule_removals;
}

}  // namespace trade_ngin
