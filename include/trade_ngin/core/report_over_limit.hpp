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

/// The labels of the report's leverage lines, each naming its denominator in plain words: the text
/// a reader of the report sees carries no formula symbol. "The
/// account value" is the day's current portfolio value and nothing else in the report.
inline constexpr const char* kReportGrossOnAccount = "Gross Leverage (on the account value)";
inline constexpr const char* kReportNetOnAccount = "Net Leverage (on the account value)";
inline constexpr const char* kReportGrossOnSizing = "Gross Leverage (on the sizing capital)";
inline constexpr const char* kReportNetOnSizing = "Net Leverage (on the sizing capital)";

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

/**
 * @brief The lines as the report's html, one `metric` div each, in this order: gross leverage on
 *        the sizing capital, net leverage on the sizing capital, the per-name cap, over limit
 *        after rounding, over limit by hold, CAP. With no reading: one line that says so.
 */
inline std::string over_limit_report_html(const OverLimitReport& report) {
    auto times = [](double value) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.2fx", value);
        return std::string(buf);
    };
    auto line = [](const std::string& label, const std::string& value) {
        return "<div class=\"metric\"><strong>" + label + ":</strong> " + value + "</div>\n";
    };
    if (!report.has_reading) {
        return line("Leverage on the sizing capital and over-limit marks",
                    "no reading (no sized rebalance this run: no risk reading was stored for "
                    "this day)");
    }
    std::string html;
    html += line(kReportGrossOnSizing,
                 times(report.gross_leverage) + " (limit " + times(report.gross_limit) + ")");
    html += line(kReportNetOnSizing, times(report.net_leverage) + " (limit " +
                                         times(report.net_limit) + " in either direction)");
    html += line("Per-name cap", times(report.per_name_cap) + " of the sizing capital");
    if (report.over_limit_after_rounding_terms.empty()) {
        html += line("Over limit after rounding", "no mark");
    } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", report.over_limit_after_rounding_excess);
        html +=
            line("Over limit after rounding",
                 "terms " + over_limit_terms_words(report.over_limit_after_rounding_terms) +
                     "; excess " + buf + " (in units of the largest stored per-contract weight)");
    }
    if (report.over_limit_by_hold_terms.empty()) {
        html += line("Over limit by hold", "no mark");
    } else {
        html += line(
            "Over limit by hold",
            "terms " + over_limit_terms_words(report.over_limit_by_hold_terms) + "; symbols " +
                (report.over_limit_by_hold_symbols.empty() ? std::string("none named")
                                                           : report.over_limit_by_hold_symbols));
    }
    html += line("Held beyond the per-name cap",
                 over_limit_terms_name_cap(report.over_limit_by_hold_terms)
                     ? "marked: a held row is beyond the per-name cap " + times(report.per_name_cap)
                     : std::string("no mark"));
    return html;
}

}  // namespace trade_ngin
