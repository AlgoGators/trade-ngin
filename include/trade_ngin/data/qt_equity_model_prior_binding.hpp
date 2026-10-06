#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <optional>
#include <string>

namespace trade_ngin {
// One immutable trading.qt_equity_model_prior_bindings row (migration 024):
// the MODEL publication P2 and the verified-desk prior it consumed. Written only
// by the VerifiedEquity MODEL publisher, inside its publishing transaction.
struct QtEquityModelPriorBinding {
    std::string publication_id;
    std::string book_id;
    std::string source_day;  // the publication (valuation) day D, not the prior's S
    std::string strategy_id;
    std::string decision_id;
    std::string finalization_id;
    std::string finalization_digest;
    std::string finalization_source_id;
    std::string finalization_source_digest;
    std::optional<std::string> actions_source_id;      // v2 action frame's actions_source only
    std::optional<std::string> actions_source_digest;
    nlohmann::json replay_reference;
    std::string replay_reference_digest;  // sha256(canonical_qt_desk_input_json(replay_reference))
};

// Pure. The replay reference must be the publisher's re-proved one
// (qt-equity-model-prior/v1 action-free, or /v2 with an action frame). Anything
// else, including a book/day mismatch, refuses with qt_equity_model_prior_binding_unavailable.
Result<QtEquityModelPriorBinding> derive_qt_equity_model_prior_binding(
    const std::string& publication_id,const std::string& book_id,
    const std::string& publication_day,const nlohmann::json& replay_reference);

// Requires the qt_equity_model_prior_binding_v1 storage capability (version 1)
// and the binding relation; inserts exactly one row in the caller's transaction.
Result<void> record_qt_equity_model_prior_binding(pqxx::work&,const QtEquityModelPriorBinding&);
}
