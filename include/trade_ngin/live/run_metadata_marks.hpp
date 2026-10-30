// include/trade_ngin/live/run_metadata_marks.hpp
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/**
 * @brief The portfolio-scope risk REFUSE of the last process_market_data call, if there was one.
 *
 * A risk REFUSE is discovered inside
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
 * REFUSE and its error, so it is seen here and in risk_decisions_json()'s
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

/**
 * @brief The run's portfolio_config JSON with a failed STRICT assertion written into it. The
 *        assertion runs after the day's live_run_metadata row
 *        was written and before anything else is stored, so a fired assertion leaves a row for a
 *        run that stored no book. As with a risk refusal the row records a run
 *        that did happen, so it is marked, not deleted; the watchdog reads the mark. Every other
 *        key of the first upsert is kept verbatim.
 */
inline nlohmann::json mark_strict_assertion(nlohmann::json portfolio_config,
                                            const std::vector<std::string>& unpriced_book_changes) {
    portfolio_config["strict_assertion"] = {
        {"reason",
         "book change(s) with no T-1 price and no execution remained after the rollback; the "
         "run stored no book"},
        {"unpriced_book_changes", unpriced_book_changes}};
    return portfolio_config;
}

/**
 * @brief A run's trading.live_run_metadata write, built where the run's configuration is known
 *        and written at the first point where the run stores anything else (T-8D-2 R48). A run
 *        that refuses before that point never calls write_once(), so it leaves no row claiming a
 *        run. write_once() writes on its first call only: the runner calls it on every path that
 *        reaches a store, and the row is written once, before the first of them.
 */
class DeferredRunRecord {
public:
    /// Hold the write. The callable owns copies of what it writes (capture by value), so the
    /// row is the one built at this point whatever the run changes afterwards.
    void hold(std::function<void()> write) {
        write_ = std::move(write);
        written_ = false;
    }
    /// Run the held write, the first time only. Nothing held: nothing written.
    void write_once() {
        if (written_ || !write_)
            return;
        written_ = true;
        write_();
    }
    bool written() const {
        return written_;
    }

private:
    std::function<void()> write_;
    bool written_ = false;
};

}  // namespace trade_ngin
