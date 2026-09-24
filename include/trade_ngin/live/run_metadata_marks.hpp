// include/trade_ngin/live/run_metadata_marks.hpp
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/**
 * @brief The portfolio-scope risk REFUSE of the last process_market_data call, if there was one.
 *
 * T-RISK-ARCH Q2 (ruled yes, HD 2026-09-19). A risk REFUSE is discovered inside
 * process_market_data, which the futures runners call AFTER they have written the day's
 * trading.live_run_metadata row. That row records a run that did happen, so it is not deleted;
 * it is MARKED: the runner writes the same row a second time with the refusal in its
 * portfolio_config JSON (mark_risk_refusal below). Without the mark the watchdog
 * (scripts/check_live_trading.py) reads the row as an ordinary completed run.
 *
 * Only a PORTFOLIO-scope record whose APPLIED action is REFUSE counts: a sleeve REFUSE pins
 * that sleeve and does not refuse the portfolio. The first such record is returned (the loop
 * leaves on a portfolio REFUSE, so there is at most one per phase).
 *
 * A module that FAILED and so refused the scope (PortfolioManager::refuse_on_failed_gatekeeper,
 * or the portfolio risk step itself failing, module kRiskStepModuleId) is recorded with applied
 * REFUSE and its error (T-7a commit 5), so it is seen here and in risk_decisions_json()'s
 * "outcome". Its "reason" is the error, since the module returned no decision, and "error"
 * carries it too; the runner's exit path reads the same record through
 * portfolio_risk_module_failure (risk_module_failure.hpp).
 *
 * @param records PortfolioManager::last_risk_decisions() after the runner's explicit call.
 * @return {"action", "module", "scope_id", "phase", "lap", "reason"} plus "error" when the
 *         refusal came from a failure, or nullopt.
 */
inline std::optional<nlohmann::json> portfolio_risk_refusal(
    const std::vector<RiskDecisionRecord>& records) {
    for (const auto& rec : records) {
        if (rec.scope != RiskScope::PORTFOLIO || rec.applied_action != RiskAction::REFUSE) {
            continue;
        }
        nlohmann::json j;
        j["action"] = risk_action_name(rec.applied_action);
        j["module"] = rec.module_id;
        j["scope_id"] = rec.scope_id;
        j["phase"] = risk_phase_name(rec.phase);
        j["lap"] = rec.lap;
        j["reason"] = rec.requested.reason.empty() ? rec.error : rec.requested.reason;
        if (!rec.error.empty()) j["error"] = rec.error;
        return j;
    }
    return std::nullopt;
}

/**
 * @brief The run's portfolio_config JSON with a risk refusal written into it.
 *
 * `risk_refusal` carries the refusal (portfolio_risk_refusal) and `risk_decisions` the PM's
 * full decision record (PortfolioManager::risk_decisions_json()). Every other key of the
 * first upsert is kept verbatim, so the second upsert changes nothing but the mark.
 */
inline nlohmann::json mark_risk_refusal(nlohmann::json portfolio_config,
                                        const nlohmann::json& refusal,
                                        const nlohmann::json& risk_decisions) {
    portfolio_config["risk_refusal"] = refusal;
    portfolio_config["risk_decisions"] = risk_decisions;
    return portfolio_config;
}

}  // namespace trade_ngin
