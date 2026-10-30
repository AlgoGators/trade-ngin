#pragma once

// The over-limit lines of a futures book's daily report:
// the stored book's gross and net leverage ON THE SIZING CAPITAL E_t, each beside the limit the
// overlay ran with (L_max, L_net_max) and the per-name cap L, and the day's marks from
// risk_detail: over_limit_after_rounding (terms and excess),
// over_limit_by_hold (terms and symbols) and CAP. A day with no mark says so; a day whose row
// carries no risk_detail (no sized rebalance, a refused overlay) says it has no reading and
// prints no figure.
//
// The marks are text, so the report's metric map (name -> double) cannot carry them: the runner
// hands the report this struct. The equity book runs no overlay and has no such lines.

#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#include "trade_ngin/risk/overlay_columns.hpp"
#include "trade_ngin/risk/risk_detail.hpp"
#include "trade_ngin/risk/risk_module_config.hpp"

namespace trade_ngin {

/// What the over-limit lines print for one day.
struct OverLimitReport {
    /// The day's row carries risk_detail and the two leverage readings. False: no figure and no
    /// mark is printed, the lines say there is no reading.
    bool has_reading{false};
    double gross_leverage{0.0};  ///< the stored book's L on E_t (the row's overlay_gross_leverage)
    double net_leverage{0.0};    ///< the stored book's L_net on E_t, signed (overlay_net_leverage)
    double gross_limit{0.0};     ///< L_max, the configured value the overlay ran with
    double net_limit{0.0};       ///< L_net_max
    double per_name_cap{0.0};    ///< L
    std::string over_limit_after_rounding_terms;   ///< ';'-separated, empty when none
    double over_limit_after_rounding_excess{0.0};  ///< in units of the largest non-zero stored u_i
    std::string over_limit_by_hold_terms;          ///< ';'-separated, CAP included, empty when none
    std::string over_limit_by_hold_symbols;        ///< sorted, space-separated, empty when none
};

/**
 * @brief The day's over-limit report from what the runner holds at the email call.
 *
 * @param day          the pass's record of the rebalance (the source of the row's risk_detail)
 * @param columns      the row's overlay columns: the overlay's readings of the STORED book
 * @param modules      the book's portfolio-scope risk modules, as its PortfolioManager was given
 *                     them; the module that carries the overlay's limits gives L_max and L_net_max
 * @param per_name_cap the book's cap L (PortfolioConfig::per_name_cap)
 */
inline OverLimitReport over_limit_report(const OnePassDay& day, const OverlayColumns& columns,
                                         const std::vector<RiskModuleConfig>& modules,
                                         double per_name_cap) {
    OverLimitReport report;
    for (const auto& module : modules) {
        const auto* carver = std::get_if<CarverModuleConfig>(&module.params);
        if (carver == nullptr || !carver->overlay_limits())
            continue;
        report.gross_limit = carver->max_gross_leverage;
        report.net_limit = carver->max_net_leverage;
    }
    report.per_name_cap = per_name_cap;
    if (!day.stores_detail() || !columns.overlay_gross_leverage || !columns.overlay_net_leverage) {
        return report;
    }
    report.has_reading = true;
    report.gross_leverage = *columns.overlay_gross_leverage;
    report.net_leverage = *columns.overlay_net_leverage;
    report.over_limit_after_rounding_terms = day.over_limit_after_rounding_terms;
    report.over_limit_after_rounding_excess = day.over_limit_after_rounding_excess;
    report.over_limit_by_hold_terms = day.over_limit_by_hold_terms;
    report.over_limit_by_hold_symbols = day.over_limit_by_hold_symbols;
    return report;
}

/// True when the ';'-separated list of terms names CAP.
inline bool over_limit_terms_name_cap(const std::string& terms) {
    size_t begin = 0;
    while (begin <= terms.size()) {
        const size_t end = terms.find(';', begin);
        if (terms.substr(begin, end == std::string::npos ? std::string::npos : end - begin) ==
            "CAP") {
            return true;
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return false;
}

/// One stored term of a mark in plain words (the overlay's checks and the per-name cap): the
/// report prints no formula symbol. A term this does not know is
/// returned as stored, so nothing is ever hidden.
inline std::string over_limit_term_words(const std::string& term) {
    if (term == "R")
        return "portfolio risk";
    if (term == "R_jump")
        return "jump risk";
    if (term == "R_shock")
        return "correlation-shock risk";
    if (term == "L_g")
        return "gross leverage";
    if (term == "L_n")
        return "net leverage";
    if (term == "CAP")
        return "per-name cap";
    return term;
}

/// A stored ';'-separated list of terms as plain words, comma-separated, in the stored order.
inline std::string over_limit_terms_words(const std::string& terms) {
    std::string words;
    size_t begin = 0;
    while (begin <= terms.size()) {
        const size_t end = terms.find(';', begin);
        const std::string term =
            terms.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!term.empty())
            words += (words.empty() ? "" : ", ") + over_limit_term_words(term);
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return words;
}

}  // namespace trade_ngin
