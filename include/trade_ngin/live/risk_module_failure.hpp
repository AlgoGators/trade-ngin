// include/trade_ngin/live/risk_module_failure.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/**
 * The live futures runners' side of a portfolio risk module that could not answer.
 *
 * HD 2026-09-21, option (b): when a portfolio-scope risk module's evaluate errors (the shipped
 * futures books run one Carver module), the PortfolioManager REFUSES the scope: every strategy
 * is held at its seeded T-1 book and no orders are generated. The runner then stores the day
 * exactly as it stores any REFUSE day (the metadata row marked by run_metadata_marks.hpp, the
 * held positions, no executions, live_results), logs an ERROR, flags the email, and exits
 * NON-ZERO so the cron wrapper and the operator see a day the gate did not measure.
 *
 * T-7b-2 C10b (HD 2026-09-24 ruling 18): a SLEEVE-scope module that cannot answer (any
 * capability, or the sleeve's risk step throwing) refuses its sleeve the same way: that sleeve is
 * held at its seeded T-1 book and sends no orders while the other sleeves trade. The runner flags
 * it with the SAME exit code and email flag as a portfolio failure (a book the gate did not
 * measure was stored; the cron wrapper and the operator already read code 3 and the flag) and
 * marks the metadata row (risk_refusal, scope "sleeve") when no portfolio refusal marked it.
 */

/// The runner's exit code on a day a portfolio risk module failed and the book was held. Not 1,
/// which every refusal to START uses (no metadata row, nothing stored): this run completed and
/// stored a held book, and the code says which of the two happened.
inline constexpr int kRiskModuleFailureExitCode = 3;

/// The first PORTFOLIO-scope record of the last process_market_data call whose module failed
/// (non-empty error), or nullopt. At portfolio scope every such failure refuses the scope, so
/// this is the day's "the gate could not answer" fact.
/// @return {"module", "error", "scope_id", "phase", "lap", "applied"}
inline std::optional<nlohmann::json> portfolio_risk_module_failure(
    const std::vector<RiskDecisionRecord>& records) {
    for (const auto& rec : records) {
        if (rec.scope != RiskScope::PORTFOLIO || rec.error.empty()) continue;
        nlohmann::json j;
        j["module"] = rec.module_id;
        j["error"] = rec.error;
        j["scope_id"] = rec.scope_id;
        j["phase"] = risk_phase_name(rec.phase);
        j["lap"] = rec.lap;
        j["applied"] = risk_action_name(rec.applied_action);
        return j;
    }
    return std::nullopt;
}

/// T-7b-2 C10b: the first SLEEVE-scope record of the last process_market_data call whose module
/// (or the sleeve's risk step, kRiskStepModuleId) failed AND whose sleeve was refused for it, or
/// nullopt. The same keys as portfolio_risk_module_failure plus "scope" ("sleeve"), "action"
/// ("REFUSE") and "reason" (the error), so the object is also the metadata row's risk_refusal
/// mark.
inline std::optional<nlohmann::json> sleeve_risk_module_failure(
    const std::vector<RiskDecisionRecord>& records) {
    for (const auto& rec : records) {
        if (rec.scope != RiskScope::SLEEVE || rec.error.empty() ||
            rec.applied_action != RiskAction::REFUSE) {
            continue;
        }
        nlohmann::json j;
        j["scope"] = "sleeve";
        j["action"] = risk_action_name(rec.applied_action);
        j["module"] = rec.module_id;
        j["error"] = rec.error;
        j["reason"] = rec.error;
        j["scope_id"] = rec.scope_id;
        j["phase"] = risk_phase_name(rec.phase);
        j["lap"] = rec.lap;
        j["applied"] = risk_action_name(rec.applied_action);
        return j;
    }
    return std::nullopt;
}

/// What main() returns after the day is stored: 0, or kRiskModuleFailureExitCode when a
/// portfolio risk module failed and the book was held, or (T-7b-2 C10b) a sleeve risk module
/// failed and its sleeve was held.
inline int live_run_exit_code(const std::optional<nlohmann::json>& module_failure) {
    return module_failure.has_value() ? kRiskModuleFailureExitCode : 0;
}

/// The subject prefix a flagged email carries.
inline constexpr const char* kRiskModuleFailureSubjectFlag = "[RISK MODULE FAILED - BOOK HELD] ";

inline std::string risk_module_failure_email_subject(const std::string& subject) {
    return std::string(kRiskModuleFailureSubjectFlag) + subject;
}

namespace detail {
inline std::string html_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}
}  // namespace detail

/// The red banner a flagged email opens with.
inline std::string risk_module_failure_email_banner(const nlohmann::json& failure) {
    const std::string module = failure.value("module", std::string());
    const std::string error = failure.value("error", std::string());
    std::string html;
    html += "<div class=\"alert-note\" id=\"risk-module-failure\">\n";
    if (failure.value("scope", std::string()) == "sleeve") {
        const std::string sleeve = failure.value("scope_id", std::string());
        html += "<strong>RISK MODULE FAILED - BOOK HELD:</strong> the sleeve risk module " +
                detail::html_escape(module) + " could not evaluate sleeve " +
                detail::html_escape(sleeve) + "'s book (" + detail::html_escape(error) +
                "). That sleeve is held at the previous day's positions and sent no orders; the "
                "other sleeves traded. The run exited with code " +
                std::to_string(kRiskModuleFailureExitCode) + ".\n";
        html += "</div>\n";
        return html;
    }
    html += "<strong>RISK MODULE FAILED - BOOK HELD:</strong> the portfolio risk module " +
            detail::html_escape(module) + " could not evaluate today's book (" +
            detail::html_escape(error) +
            "). Every strategy is held at the previous day's positions and no orders were "
            "generated. The run exited with code " +
            std::to_string(kRiskModuleFailureExitCode) + ".\n";
    html += "</div>\n";
    return html;
}

/// `body` with the banner placed at the top of the report (inside the container the email
/// builder opens, EmailSender::generate_trading_report_body), or in front of the body when the
/// container is not found.
inline std::string flag_email_body_for_risk_module_failure(const std::string& body,
                                                           const nlohmann::json& failure) {
    static const std::string kContainer = "<div class=\"container\">\n";
    const std::string banner = risk_module_failure_email_banner(failure);
    const auto at = body.find(kContainer);
    if (at == std::string::npos) return banner + body;
    std::string flagged = body;
    flagged.insert(at + kContainer.size(), banner);
    return flagged;
}

}  // namespace trade_ngin
