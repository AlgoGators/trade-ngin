// include/trade_ngin/risk/risk_scale_report.hpp
//
// RA-01 (T-RISK-ARCH §2.1): live_results.risk_scale is written by a throwaway snapshot
// RiskManager that every live runner builds AFTER the trade; it never moves a contract. The
// scale that did move the book is the PortfolioManager's, recorded row by row in
// last_risk_decisions(). HD 2026-09-18: "the applied risk scale is logged per lap beside the
// reporter's value now; the stored column is T-8's to rule". The per-lap half is the PM's
// RISK_APPLIED line; this is the per-rebalance half, one line beside the stored value:
//
//   RISK_SCALE_REPORT reporter=<r> applied_cumulative=<c> laps=<n> cutting_laps=<k>
//                     binding_module=<id> [date=<YYYY-MM-DD>]
//
// Nothing here stores anything or changes a number; it only reads the record.
#pragma once

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "trade_ngin/risk/risk_module.hpp"

namespace trade_ngin {

/// The PortfolioManager's own record of one rebalance, reduced to the report's four fields.
/// Every field is read from PORTFOLIO-scope rows of phase LAP only:
///   * SLEEVE-phase rows are excluded: a sleeve SCALE multiplies one sleeve, not the book (it
///     has its own RISK_APPLIED scope=sleeve line). No shipped book has a sleeve module.
///   * POST_ROUNDING rows are excluded: the PM rejects a SCALE there, so none multiplies.
struct RiskScaleSummary {
    /// The product, in record (= lap) order, of double(applied_factor) over every row whose
    /// applied_action is SCALE: the quantised factors the book was actually multiplied by.
    /// The same doubles multiplied in the same order as the PM's RiskContext::applied, so it
    /// equals, bit for bit, the cumulative= of the rebalance's last RISK_APPLIED scope=portfolio
    /// line. 1 when nothing was multiplied. A REFUSE or REPLACE is not a scale and is not in it.
    double applied_cumulative{1.0};
    /// The number of distinct lap numbers among the rows: every lap on which the portfolio risk
    /// step ran, including a lap on an empty book (one empty_book row per module, no
    /// RISK_APPLIED line) and a lap whose risk step failed (the kRiskStepModuleId REFUSE row).
    /// 0 when the record is empty: a book with no module (`none`: the PM holds no module and
    /// records nothing) or a run with no process_market_data call.
    int laps{0};
    /// The number of those laps with a SCALE row whose applied factor is < 1. A SCALE whose
    /// quantised factor is exactly 1 multiplied by 1 and is not a cut.
    int cutting_laps{0};
    /// module_id of the SCALE row of the last cutting lap; "none" when no lap cut.
    std::string binding_module{"none"};
};

/// @param records PortfolioManager::last_risk_decisions() after the rebalance it describes.
inline RiskScaleSummary summarize_applied_risk(const std::vector<RiskDecisionRecord>& records) {
    RiskScaleSummary s;
    std::set<int> laps;
    std::set<int> cutting;
    for (const auto& rec : records) {
        if (rec.scope != RiskScope::PORTFOLIO || rec.phase != RiskPhase::LAP) continue;
        laps.insert(rec.lap);
        if (rec.applied_action != RiskAction::SCALE) continue;
        const double factor = static_cast<double>(rec.applied_factor);
        s.applied_cumulative *= factor;
        if (factor < 1.0) {
            cutting.insert(rec.lap);
            s.binding_module = rec.module_id;
        }
    }
    s.laps = static_cast<int>(laps.size());
    s.cutting_laps = static_cast<int>(cutting.size());
    return s;
}

/// The line. `reporter` is printed as given (the futures backtest passes "na": it has no
/// snapshot reporter and stores no risk figure). A non-empty `date` appends " date=<date>",
/// which the backtest passes because its log lines carry only the wall clock. applied_cumulative
/// is printed %.17g, which reads back as the same double, the precision of RISK_APPLIED.
inline std::string format_risk_scale_report(const std::string& reporter,
                                            const RiskScaleSummary& s,
                                            const std::string& date = std::string()) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", s.applied_cumulative);
    std::string line = "RISK_SCALE_REPORT reporter=" + reporter + " applied_cumulative=" + buf +
                       " laps=" + std::to_string(s.laps) +
                       " cutting_laps=" + std::to_string(s.cutting_laps) +
                       " binding_module=" + (s.binding_module.empty() ? "-" : s.binding_module);
    if (!date.empty()) line += " date=" + date;
    return line;
}

/// The live runners' form: `reporter` is the double the runner stores as live_results.risk_scale
/// (the snapshot RiskManager's recommended_scale, 1.0 when its evaluation failed), printed %.17g.
inline std::string format_risk_scale_report(double reporter, const RiskScaleSummary& s) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", reporter);
    return format_risk_scale_report(std::string(buf), s);
}

}  // namespace trade_ngin
