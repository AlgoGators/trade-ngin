#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
namespace trade_ngin {
// Pure finalization of an already admitted futures accounting day. No source
// discovery, policy grant, SQL writes, strategy execution or delivery.
Result<nlohmann::json> produce_qt_prior_finalization(
    const nlohmann::json& decision,const nlohmann::json& original_input,
    const nlohmann::json& original_output,const nlohmann::json& market_payload,
    const nlohmann::json& before_financial,const nlohmann::json& provenance);
}
