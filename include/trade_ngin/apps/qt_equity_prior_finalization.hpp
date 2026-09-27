#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
namespace trade_ngin {
// Pure mark dependency; supplied provenance never grants processing authority.
Result<nlohmann::json> produce_qt_equity_prior_finalization(
    const nlohmann::json& decision, const nlohmann::json& original_input,
    const nlohmann::json& original_output, const nlohmann::json& market_payload,
    const nlohmann::json& actions_payload, const nlohmann::json& before_financial,
    const nlohmann::json& provenance);
}
