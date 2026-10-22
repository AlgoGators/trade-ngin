#pragma once

// The two risk lines of the daily report and of the console (HD 2026-10-10): the overlay's
// expected risk of the stored book as a percent of the sizing capital beside the risk target,
// and the one-day 95 percent VaR in dollars. They replace the line "Portfolio VaR", which
// printed trading.live_results.portfolio_var: a price-weighted figure with no contract
// multiplier, not a value-at-risk and not the book's risk (its stored value is unchanged).
//
// The runner puts the figures in the report's metric map under the keys below; a figure with no
// value (no sized rebalance, a refused overlay, a blind window) is left out and its line is not
// printed in the report (the console prints N/A).

#include <cstdio>
#include <map>
#include <optional>
#include <string>

namespace trade_ngin {

inline constexpr const char* kReportExpectedRisk = "Expected Risk";  ///< percent of the sizing capital, a year
inline constexpr const char* kReportRiskTarget = "Risk Target";      ///< percent, the book's tau
inline constexpr const char* kReportVar95 = "VaR 95 1-Day";          ///< dollars
inline constexpr const char* kReportRiskCovered = "Risk Contracts Covered";  ///< inside the covariance
inline constexpr const char* kReportRiskHeld = "Risk Contracts Held";        ///< the book's contracts

/// The figures as the report's metrics. `expected_risk` is the overlay's R as a fraction of the
/// sizing capital, `tau` the risk target as a fraction, `var_95_1d` dollars.
inline std::map<std::string, double> report_risk_metrics(const std::optional<double>& expected_risk,
                                                         double tau,
                                                         const std::optional<double>& var_95_1d,
                                                         int contracts_in_risk,
                                                         int contracts_held) {
    std::map<std::string, double> metrics;
    if (expected_risk) {
        metrics[kReportExpectedRisk] = *expected_risk * 100.0;
        metrics[kReportRiskTarget] = tau * 100.0;
        metrics[kReportRiskCovered] = static_cast<double>(contracts_in_risk);
        metrics[kReportRiskHeld] = static_cast<double>(contracts_held);
    }
    if (var_95_1d) metrics[kReportVar95] = *var_95_1d;
    return metrics;
}

/// The two values as text; each is empty when the metrics carry no figure for it.
///   expected_risk  "7.91% of the sizing capital (target 20.00%; 15 of 15 contracts)"
///   var_95_1d      "$4,618.64"
struct ReportRiskLines {
    std::string expected_risk;
    std::string var_95_1d;
};

inline ReportRiskLines report_risk_lines(const std::map<std::string, double>& metrics) {
    ReportRiskLines lines;
    char buf[160];
    const auto risk = metrics.find(kReportExpectedRisk);
    const auto target = metrics.find(kReportRiskTarget);
    if (risk != metrics.end() && target != metrics.end()) {
        std::snprintf(buf, sizeof(buf), "%.2f%% of the sizing capital (target %.2f%%", risk->second,
                      target->second);
        lines.expected_risk = buf;
        const auto covered = metrics.find(kReportRiskCovered);
        const auto held = metrics.find(kReportRiskHeld);
        if (covered != metrics.end() && held != metrics.end()) {
            std::snprintf(buf, sizeof(buf), "; %d of %d contracts", static_cast<int>(covered->second),
                          static_cast<int>(held->second));
            lines.expected_risk += buf;
        }
        lines.expected_risk += ")";
    }
    const auto var = metrics.find(kReportVar95);
    if (var != metrics.end()) {
        std::snprintf(buf, sizeof(buf), "%.2f", var->second);
        std::string text = buf;
        const size_t first = text[0] == '-' ? 1 : 0;
        for (int at = static_cast<int>(text.find('.')) - 3; at > static_cast<int>(first); at -= 3) {
            text.insert(static_cast<size_t>(at), ",");
        }
        lines.var_95_1d = "$" + text;
    }
    return lines;
}

/// The console's two lines (each ends with a newline); N/A where there is no figure.
inline std::string report_risk_console(const std::map<std::string, double>& metrics) {
    const ReportRiskLines lines = report_risk_lines(metrics);
    return "Expected Risk: " + (lines.expected_risk.empty() ? std::string("N/A") : lines.expected_risk) +
           "\n1-Day 95% VaR: " + (lines.var_95_1d.empty() ? std::string("N/A") : lines.var_95_1d) +
           "\n";
}

}  // namespace trade_ngin
