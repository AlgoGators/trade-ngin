#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <stdexcept>
#include "trade_ngin/strategy/equity_strategy_builder.hpp"

namespace trade_ngin {
// Storage names are derived separately; the approved snapshot remains untouched.
inline nlohmann::json live_config_storage_strategies(const nlohmann::json& snapshot,
                                                     const std::string& engine) {
    const auto& source=snapshot.at("strategies");
    if (engine!="LIVE_EQUITY_MEAN_REVERSION" || !source.contains("MEAN_REVERSION")) return source;
    const auto selected=apps::collect_enabled_equity_strategies(source,"enabled_live");
    if (selected.is_error()) throw std::runtime_error("config_owner_map_invalid");
    const auto plan=apps::build_equity_live_book_plan(selected.value());
    if (plan.is_error() || !plan.value().legacy_single || plan.value().combined_strategy_id!=engine)
        throw std::runtime_error("config_owner_map_invalid");
    auto result=source;
    result.erase("MEAN_REVERSION");
    if (result.contains("EQUITY_MEAN_REVERSION")) throw std::runtime_error("config_owner_map_collision");
    result["EQUITY_MEAN_REVERSION"]=source.at("MEAN_REVERSION");
    return result;
}
inline nlohmann::json live_config_source_owner_map(const nlohmann::json& snapshot,
                                                   const std::string& engine) {
    (void)live_config_storage_strategies(snapshot,engine);
    nlohmann::json result=nlohmann::json::object();
    for (const auto& [name,definition]:snapshot.at("strategies").items())
        if (definition.value("enabled_live",false))
            result[name]=(engine=="LIVE_EQUITY_MEAN_REVERSION" && name=="MEAN_REVERSION")
                ? "EQUITY_MEAN_REVERSION" : name;
    return result;
}
}
