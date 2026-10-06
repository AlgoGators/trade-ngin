#pragma once
#include "trade_ngin/apps/equity_model_action_frame.hpp"
namespace trade_ngin {
// Called only after the existing original/successor/current-physical proof.
// Uses the caller's book-fenced transaction; publisher repeats it before writes.
Result<EquityModelActionFrame> capture_equity_model_action_frame(
    pqxx::work&,const VerifiedEquityModelPrior&,const EquityModelPriorOwner&,
    const nlohmann::json& original_adjustments,const nlohmann::json& finalization);
}
