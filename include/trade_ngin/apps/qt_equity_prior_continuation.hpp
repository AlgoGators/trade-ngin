#pragma once
#include "trade_ngin/core/error.hpp"
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
#include <string>
#include <vector>
namespace trade_ngin {
// Equity day 2 (verified desk prior): the D accounting market A, bound to the
// decision's model P2, may stand in for F's finalization-only market M only
// when every rule (a)-(d) holds. Rows are to_jsonb() images; nothing here
// grants processing authority, and the same-market path never reaches it.
// Contract v1 names the field type "Json"; the project has no public alias (every
// .cpp declares its own), so it is spelled nlohmann::json. Semantics are identical.
struct QtEquityPriorContinuationInputs {
    nlohmann::json decision;             // D decision row (to_jsonb)
    nlohmann::json prior_decision;       // S decision row
    nlohmann::json finalization_market;  // M row (qt_desk_market_sources, finalization-only schema)
    nlohmann::json input_market;         // A row (qt_desk_market_sources, qt-equity-accounting-market/v1, bound to P2)
    nlohmann::json finalization;         // F row (qt_desk_finalizations)
    nlohmann::json anchor;               // qt_desk_finalization_sources row (derived v2 anchor)
    nlohmann::json binding;              // qt_equity_model_prior_bindings row for d.model_publication_id
    nlohmann::json actions_row;          // qt_equity_desk_evidence_sources row named by A.actions_source_id
    std::vector<std::string> candidate_action_sources; // current non-empty D action sources (the MODEL's query) for A's producer/policy/revision
};
// Pure: no pqxx, no I/O. Fails closed with qt_equity_prior_continuation_unavailable.
Result<void> validate_qt_equity_verified_prior_continuation(const QtEquityPriorContinuationInputs&);
// Loads M, the binding, the prior decision, the actions row and the candidates
// (FOR SHARE) inside the caller's transaction, then calls the pure validator.
Result<void> verify_qt_equity_prior_continuation(pqxx::work& tx, const nlohmann::json& decision,
    const nlohmann::json& input_market, const nlohmann::json& finalization_row, const nlohmann::json& anchor_row);
}
