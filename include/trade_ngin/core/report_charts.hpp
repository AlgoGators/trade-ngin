// include/trade_ngin/core/report_charts.hpp
#pragma once

#include <string>
#include <vector>

#include "trade_ngin/core/gnuplot_run.hpp"

namespace trade_ngin {
namespace core {

/**
 * What the report says about its charts.
 *
 * A chart that is not in the report is named there in one line with the reason, so a reader
 * never has to guess whether a chart is missing: "Equity Curve: not shown (the chart program did
 * not finish)", "Portfolio Composition: not shown (no positions)". The run's log gets one line:
 * how many charts were asked for, how many were drawn, and which ones were not.
 */
class ChartLedger {
public:
    /// Records one chart and returns the line to print in its place: empty when it was drawn.
    std::string record(const std::string& name, bool drawn, ChartStatus status,
                       const std::string& failed_reason, const std::string& nothing_reason) {
        ++expected_;
        if (drawn) {
            ++drawn_;
            return "";
        }
        const bool failed = status == ChartStatus::Failed;
        const std::string& reason = failed ? failed_reason : nothing_reason;
        missing_.push_back(name + (failed ? " (failed: " : " (") + reason + ")");
        return "<p style=\"margin: 12px 0; color: #666; font-size: 13px;\">" + name +
               ": not shown (" + reason + ")</p>\n";
    }

    int expected() const { return expected_; }
    int drawn() const { return drawn_; }

    std::string summary() const {
        std::string line = "charts: " + std::to_string(expected_) + " expected, " +
                           std::to_string(drawn_) + " drawn";
        for (std::size_t i = 0; i < missing_.size(); ++i) {
            line += (i == 0 ? "; not shown: " : ", ") + missing_[i];
        }
        return line;
    }

private:
    int expected_{0};
    int drawn_{0};
    std::vector<std::string> missing_;
};

}  // namespace core
}  // namespace trade_ngin
