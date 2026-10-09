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
 * Sleeve scope is not covered: a sleeve module's failure refuses only that sleeve and only when
 * the module is REFUSE-capable (unchanged), so it is not a run failure.
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

/// What main() returns after the day is stored: 0, or kRiskModuleFailureExitCode when a
/// portfolio risk module failed and the book was held.
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
