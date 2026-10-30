// include/trade_ngin/live/report_rolls_json.hpp
#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "trade_ngin/core/report_rolls.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin::live {

/**
 * @brief What the book e-mail says about rolls with per_contract_rolls on, for the report module
 *        (its --rolls): the texts report_rolls.hpp makes, so the module lays them out and computes
 *        none of them.
 *
 *   lines            one per roll, from its closing leg's split and both legs' costs (report_roll_line),
 *                    in sleeve order then leg order; a leg with no split gets none
 *   rollover         the held contracts inside their roll window: label, held, target, by (MM-DD),
 *                    deadline (MM-DD) and its kind ("first notice" or "last trading day")
 *   front_month      root -> the contract held (the Symbols Reference's Front Month column)
 *   contract_months  root -> the cycle's months ("Mar Jun Sep Dec"), only for a root with a cycle
 *   footer           the footer's futures sentence (kReportRollsFooter)
 *   roll_bar_note    the settled table's note for a row whose bar was a per-contract roll bar
 *   roll_bar_symbols the settled day's per-contract roll symbols ("SI.v.0")
 */
inline nlohmann::json report_rolls_json(
    const ReportRolls& rolls,
    const std::unordered_map<std::string, std::vector<ExecutionReport>>& sleeve_executions,
    const std::unordered_set<std::string>& roll_bar_symbols) {
    nlohmann::json out = nlohmann::json::object();
    if (!rolls.per_contract_rolls) return out;

    nlohmann::json lines = nlohmann::json::array();
    std::vector<std::string> sleeves;
    for (const auto& [sleeve, executions] : sleeve_executions) sleeves.push_back(sleeve);
    std::sort(sleeves.begin(), sleeves.end());
    for (const auto& sleeve : sleeves) {
        const auto& executions = sleeve_executions.at(sleeve);
        for (const auto& closing : executions) {
            const bool is_closing = closing.exec_id.size() >= 3 &&
                                    closing.exec_id.compare(closing.exec_id.size() - 3, 3, "_RC") == 0;
            if (closing.execution_type != ExecutionType::ROLL || (!is_closing && !closing.roll_bar_move))
                continue;
            double cost = closing.total_transaction_costs.as_double();
            const std::string opening_id =
                is_closing ? closing.exec_id.substr(0, closing.exec_id.size() - 3) + "_RO" : std::string();
            for (const auto& opening : executions) {
                if (opening.execution_type == ExecutionType::ROLL && !opening_id.empty() &&
                    opening.exec_id == opening_id && opening.symbol == closing.symbol) {
                    cost += opening.total_transaction_costs.as_double();
                    break;
                }
            }
            const auto names = rolls.names.find(closing.exec_id);
            const std::string line = report_roll_line(
                names == rolls.names.end() ? ReportRollNames{} : names->second, closing.symbol, closing,
                cost);
            if (!line.empty()) lines.push_back(line);
        }
    }
    out["lines"] = lines;

    nlohmann::json rollover = nlohmann::json::array();
    nlohmann::json front_month = nlohmann::json::object();
    nlohmann::json contract_months = nlohmann::json::object();
    for (const auto& [root, o] : rolls.outlook) {
        if (!o.held.empty()) front_month[root] = o.held;
        if (!o.cycle.empty()) contract_months[root] = report_cycle_months(o.cycle);
        if (o.in_window && !o.held.empty()) {
            rollover.push_back({{"label", o.label.empty() ? o.root : o.label},
                                {"held", o.held},
                                {"target", o.target.empty() ? std::string("the next contract") : o.target},
                                {"by", report_month_day(o.roll_by)},
                                {"deadline", report_month_day(o.deadline)},
                                {"deadline_kind", o.first_notice ? "first notice" : "last trading day"}});
        }
    }
    out["rollover"] = rollover;
    out["front_month"] = front_month;
    out["contract_months"] = contract_months;
    out["footer"] = kReportRollsFooter;
    out["roll_bar_note"] = kReportRollBarNote;
    std::vector<std::string> symbols(roll_bar_symbols.begin(), roll_bar_symbols.end());
    std::sort(symbols.begin(), symbols.end());
    out["roll_bar_symbols"] = symbols;
    return out;
}

}  // namespace trade_ngin::live
