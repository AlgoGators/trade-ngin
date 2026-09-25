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

#include <cmath>
#include <cstdio>
#include <map>
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

// T-7b-2 C9a (T-VOL C4): the DELIVERED cut beside the requested one. The gate's request
// (applied_cumulative above) is what the loop multiplied the book by; what reaches the stored book
// also depends on lap 2's buffer and the rounding (T-VOL section 3.2: 0.747 of the lap-1 book in
// notional on cut days against a 0.870 request). One line per rebalance, printed right after the
// RISK_SCALE_REPORT line and only where it is printed:
//
//   RISK_DELIVERED requested=<r> delivered=<d> final_gross=<g> lap1_gross=<g1> unpriced=<n>
//                  [date=<YYYY-MM-DD>]
//
// Log only: nothing stores it and no number moves.

/// The PortfolioManager's measurement of one rebalance (PortfolioManager::last_delivered_cut()).
/// Books are the ACCOUNT book: per symbol the sum of every strategy's contracts, the book the
/// portfolio risk gate reads. Both books are valued at ONE notional per contract per symbol, taken
/// once at the end of the call, so the ratio compares quantities, not two sets of prices.
struct DeliveredCut {
    /// A lap-1 book was captured: the account book right after lap 1's optimizer step (the book
    /// lap 1's portfolio risk step reads; with the optimizer off, the strategies' targets).
    bool has_lap1{false};
    /// The call reached its end, so the final book exists: the account book of the positions the
    /// runner stores (get_strategy_positions(), after the backtest's session hold).
    bool has_final{false};
    /// sum over symbols of |contracts| x notional per contract, lap-1 book.
    double lap1_gross{0.0};
    /// The same over the final book.
    double final_gross{0.0};
    /// Symbols holding a non-zero quantity in either book with no notional per contract (left
    /// out of both grosses).
    int unpriced{0};
};

/// sum over the book of |q| x notional_per_contract[symbol], symbols in name order. A symbol with
/// q == 0 adds nothing; a symbol with q != 0 and no positive notional is added to `unpriced` and
/// left out.
inline double delivered_gross_notional(const std::map<std::string, double>& book,
                                       const std::map<std::string, double>& notional_per_contract,
                                       std::set<std::string>& unpriced) {
    double gross = 0.0;
    for (const auto& [symbol, q] : book) {
        if (q == 0.0) continue;
        auto it = notional_per_contract.find(symbol);
        if (it == notional_per_contract.end() || !(it->second > 0.0)) {
            unpriced.insert(symbol);
            continue;
        }
        gross += std::fabs(q) * it->second;
    }
    return gross;
}

/// Both grosses at the same notionals. `lap1_book` is null when no lap-1 book was captured.
inline DeliveredCut measure_delivered_cut(const std::map<std::string, double>* lap1_book,
                                          const std::map<std::string, double>& final_book,
                                          const std::map<std::string, double>& notional_per_contract) {
    DeliveredCut d;
    std::set<std::string> unpriced;
    if (lap1_book != nullptr) {
        d.has_lap1 = true;
        d.lap1_gross = delivered_gross_notional(*lap1_book, notional_per_contract, unpriced);
    }
    d.has_final = true;
    d.final_gross = delivered_gross_notional(final_book, notional_per_contract, unpriced);
    d.unpriced = static_cast<int>(unpriced.size());
    return d;
}

/// The line. Every number is printed %.17g (it reads back as the same double).
///   requested    RiskScaleSummary::applied_cumulative, the same double RISK_SCALE_REPORT prints.
///   delivered    final_gross / lap1_gross; `na` when there is no lap-1 book, no final book, or the
///                lap-1 book's gross is 0 (an EMPTY lap-1 book: no symbol, or every quantity 0 --
///                there is nothing to cut, so no ratio; final_gross is still printed).
///   final_gross  `na` when the measurement did not run: no call this cycle (the backtest's
///                all-JUNK cycle), or the call returned before its end (an error; the measurement
///                runs once, at the end of the call, so neither book is valued then).
///   lap1_gross   `na` in the same cases, and when the call ended without a lap-1 book; 0 for an
///                empty lap-1 book.
///   unpriced     DeliveredCut::unpriced.
/// A non-empty `date` appends " date=<date>" (the backtest's form).
inline std::string format_risk_delivered(const RiskScaleSummary& s, const DeliveredCut& d,
                                         const std::string& date = std::string()) {
    auto num = [](double v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", v);
        return std::string(buf);
    };
    const bool ratio = d.has_lap1 && d.has_final && d.lap1_gross > 0.0;
    std::string line = "RISK_DELIVERED requested=" + num(s.applied_cumulative) +
                       " delivered=" + (ratio ? num(d.final_gross / d.lap1_gross) : "na") +
                       " final_gross=" + (d.has_final ? num(d.final_gross) : "na") +
                       " lap1_gross=" + (d.has_lap1 ? num(d.lap1_gross) : "na") +
                       " unpriced=" + std::to_string(d.unpriced);
    if (!date.empty()) line += " date=" + date;
    return line;
}

}  // namespace trade_ngin
