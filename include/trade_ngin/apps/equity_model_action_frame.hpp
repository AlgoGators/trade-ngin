#pragma once
#include "trade_ngin/apps/equity_model_prior.hpp"
#include <vector>
namespace trade_ngin {
// Select action evidence only for actual holdings; retain zero rows in the prior.
std::vector<std::string> equity_model_action_held_symbols(
    const std::unordered_map<std::string,Position>&);
// Pure derivation prerequisite only. SQL caller must verify original/S successor,
// physical state and governed source authority; publisher recaptures all operands.
// This never writes/restates S rows or authorizes a trade.
struct EquityModelActionFrame {
    std::unordered_map<std::string,Position> positions;
    nlohmann::json replay_reference;
    nlohmann::json document;
    std::string digest;
    int original_action_count{0};
    int successor_action_count{0};
    std::string original_action_digest;
    std::string successor_action_digest;
};
Result<EquityModelActionFrame> derive_equity_model_action_frame(
    const VerifiedEquityModelPrior&,const EquityModelPriorOwner&,
    const nlohmann::json& original_adjustments,
    const nlohmann::json& actions_source,const nlohmann::json& execution_policy,
    const nlohmann::json& raw_capture);
}
