// src/risk/risk_module.cpp
#include "trade_ngin/risk/risk_module.hpp"
#include <algorithm>

namespace trade_ngin {

const char* risk_action_name(RiskAction a) {
    switch (a) {
        case RiskAction::NONE:
            return "NONE";
        case RiskAction::SCALE:
            return "SCALE";
        case RiskAction::WARN:
            return "WARN";
        case RiskAction::REFUSE:
            return "REFUSE";
        case RiskAction::REPLACE:
            return "REPLACE";
    }
    return "NONE";
}

const char* risk_term_name(RiskTerm t) {
    switch (t) {
        case RiskTerm::COMPOSITION:
            return "composition";
        case RiskTerm::MAGNITUDE:
            return "magnitude";
        case RiskTerm::PATH:
            return "path";
        case RiskTerm::CUSTOM:
            return "custom";
    }
    return "custom";
}

const char* risk_scope_name(RiskScope s) {
    return s == RiskScope::SLEEVE ? "sleeve" : "portfolio";
}

const char* risk_phase_name(RiskPhase p) {
    switch (p) {
        case RiskPhase::REBALANCE_START:
            return "rebalance_start";
        case RiskPhase::SLEEVE:
            return "sleeve";
        case RiskPhase::LAP:
            return "lap";
        case RiskPhase::POST_ROUNDING:
            return "post_rounding";
    }
    return "lap";
}

namespace {

// REFUSE > REPLACE > SCALE > WARN > NONE
int severity(RiskAction a) {
    switch (a) {
        case RiskAction::NONE:
            return 0;
        case RiskAction::WARN:
            return 1;
        case RiskAction::SCALE:
            return 2;
        case RiskAction::REPLACE:
            return 3;
        case RiskAction::REFUSE:
            return 4;
    }
    return 0;
}

nlohmann::json metrics_json(const RiskResult& r) {
    return nlohmann::json{{"risk_exceeded", r.risk_exceeded},
                          {"recommended_scale", r.recommended_scale},
                          {"portfolio_multiplier", r.portfolio_multiplier},
                          {"jump_multiplier", r.jump_multiplier},
                          {"correlation_multiplier", r.correlation_multiplier},
                          {"leverage_multiplier", r.leverage_multiplier},
                          {"portfolio_var", r.portfolio_var},
                          {"jump_risk", r.jump_risk},
                          {"correlation_risk", r.correlation_risk},
                          {"gross_leverage", r.gross_leverage},
                          {"net_leverage", r.net_leverage}};
}

}  // namespace

nlohmann::json build_risk_decisions_json(const std::vector<nlohmann::json>& modules,
                                         const std::vector<RiskDecisionRecord>& records) {
    nlohmann::json decisions = nlohmann::json::array();
    RiskAction outcome = RiskAction::NONE;
    std::set<std::string> pinned;
    int laps = 0;

    for (const auto& rec : records) {
        nlohmann::json row;
        row["phase"] = risk_phase_name(rec.phase);
        row["lap"] = rec.lap;
        row["scope"] = risk_scope_name(rec.scope);
        row["scope_id"] = rec.scope_id;
        row["module"] = rec.module_id;
        row["requested"] = {{"action", risk_action_name(rec.requested.action)},
                            {"scale", rec.requested.scale},
                            {"reason", rec.requested.reason},
                            {"blind", rec.requested.blind}};
        row["applied"] = {{"action", risk_action_name(rec.applied_action)},
                          {"factor", static_cast<double>(rec.applied_factor)},
                          {"factor_raw", rec.applied_factor.raw_value()}};
        if (rec.requested.metrics) {
            row["metrics"] = metrics_json(*rec.requested.metrics);
        }
        row["empty_book"] = rec.empty_book;
        if (!rec.error.empty()) {
            row["error"] = rec.error;
        }
        decisions.push_back(std::move(row));

        if (severity(rec.applied_action) > severity(outcome)) {
            outcome = rec.applied_action;
        }
        if (rec.applied_action == RiskAction::REFUSE || rec.applied_action == RiskAction::REPLACE) {
            pinned.insert(rec.scope_id);
        }
        if (rec.phase == RiskPhase::LAP) {
            laps = std::max(laps, rec.lap);
        }
    }

    nlohmann::json j;
    j["modules"] = modules.empty() ? nlohmann::json::array() : nlohmann::json(modules);
    j["decisions"] = std::move(decisions);
    j["outcome"] = {{"action", risk_action_name(outcome)},
                    {"refused", outcome == RiskAction::REFUSE},
                    {"pinned_scopes", std::vector<std::string>(pinned.begin(), pinned.end())},
                    {"laps", laps}};
    return j;
}

}  // namespace trade_ngin
