#pragma once

// A futures sleeve's risk_target, idm and vol_lookback_short are read from its "config" object in
// portfolio.json and are REQUIRED. The first sleeve's risk_target is the tau the risk overlay's
// three limits are ratios to, so a runner's own fallback value would move the limits without a
// word. A missing or mistyped key refuses the run at load and names the key.

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

/// The hand-over of portfolio.json's carry block to the book's first sleeve, the
/// one place it crosses from the loader to a sleeve, called by every futures runner next to
/// hand_over_trading_rule_removals. Carry off (the block absent, or weight 0) hands over weight 0
/// and the sleeve is today's.
/// The carry block as a backtest stores it in backtest.run_metadata's portfolio_config (fix round 1,
/// audit A S3): written by the two backtest runners only when carry is on, so a carry-off run's
/// stored metadata is today's and a carry-on run says which carry it ran. A live run stores it with
/// the whole file (trading.live_run_metadata.settings_used).
inline nlohmann::json carry_config_json(const CarryConfig& carry) {
    return {{"weight", carry.weight}, {"spans", carry.spans}, {"scalar", carry.scalar}};
}

inline void hand_over_carry(const AppConfig& app_config, TrendFollowingConfig& out) {
    if (!app_config.carry.enabled()) {
        out.carry_weight = 0.0;
        return;
    }
    out.carry_weight = app_config.carry.weight;
    out.carry_spans = app_config.carry.spans;
    out.carry_scalar = app_config.carry.scalar;
}

}  // namespace trade_ngin
