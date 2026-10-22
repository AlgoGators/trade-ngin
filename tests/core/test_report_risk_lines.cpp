// tests/core/test_report_risk_lines.cpp
//
// HD 2026-10-10: the report and the console print the overlay's expected risk of the stored book
// as a percent of the sizing capital beside the risk target, and the one-day 95 percent VaR in
// dollars, in place of "Portfolio VaR" (trading.live_results.portfolio_var, a price-weighted
// figure with no contract multiplier, still stored unchanged).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include "trade_ngin/core/report_risk_lines.hpp"

using namespace trade_ngin;

namespace {

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

}  // namespace

TEST(ReportRiskLines, TheExpectedRiskIsAPercentOfTheSizingCapitalBesideTheTarget) {
    // The overlay reads R = 0.0790830572 on a target of 0.20; 15 of the book's 15 contracts are
    // inside the covariance; the VaR is 4,218.027997 dollars.
    const auto metrics = report_risk_metrics(0.0790830572, 0.20, 4218.027997, 15, 15);
    EXPECT_NEAR(metrics.at("Expected Risk"), 7.90830572, 1e-9);
    EXPECT_NEAR(metrics.at("Risk Target"), 20.0, 1e-12);
    EXPECT_NEAR(metrics.at("VaR 95 1-Day"), 4218.027997, 1e-9);
    const ReportRiskLines lines = report_risk_lines(metrics);
    EXPECT_EQ(lines.expected_risk, "7.91% of the sizing capital (target 20.00%; 15 of 15 contracts)");
    EXPECT_EQ(lines.var_95_1d, "$4,218.03");
    EXPECT_EQ(report_risk_console(metrics),
              "Expected Risk: 7.91% of the sizing capital (target 20.00%; 15 of 15 contracts)\n"
              "1-Day 95% VaR: $4,218.03\n");
    // A contract outside the covariance shows in the count.
    EXPECT_EQ(report_risk_lines(report_risk_metrics(0.25, 0.25, 987.6, 14, 16)).expected_risk,
              "25.00% of the sizing capital (target 25.00%; 14 of 16 contracts)");
    EXPECT_EQ(report_risk_lines(report_risk_metrics(0.25, 0.25, 987.6, 14, 16)).var_95_1d, "$987.60");
    EXPECT_EQ(report_risk_lines(report_risk_metrics(0.1, 0.2, 1234567.891, 1, 1)).var_95_1d,
              "$1,234,567.89");
}

TEST(ReportRiskLines, ADayWithNoFigureCarriesNoKeyAndTheConsoleSaysNotAvailable) {
    // No sized rebalance, a refused overlay or a blind window: no risk reading and no VaR.
    const auto none = report_risk_metrics(std::nullopt, 0.20, std::nullopt, 0, 0);
    EXPECT_TRUE(none.empty());
    const ReportRiskLines lines = report_risk_lines(none);
    EXPECT_TRUE(lines.expected_risk.empty());
    EXPECT_TRUE(lines.var_95_1d.empty());
    EXPECT_EQ(report_risk_console(none), "Expected Risk: N/A\n1-Day 95% VaR: N/A\n");
    // The old key alone prints nothing.
    EXPECT_TRUE(report_risk_lines({{"Portfolio VaR", 7.12}}).expected_risk.empty());
}

// The runners are `main()`s: what is tested is the structure in the source.
TEST(ReportRiskLines, TheFuturesRunnersNoLongerPrintOrMailThePriceWeightedFigure) {
    for (const char* runner :
         {"apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(src.find("strategy_metrics[\"Portfolio VaR\"]"), std::string::npos) << runner;
        EXPECT_EQ(src.find("std::cout << \"Volatility: \""), std::string::npos)
            << runner << ": the console still prints portfolio_var as a volatility";
        EXPECT_EQ(src.find("\"Volatility: N/A\""), std::string::npos) << runner;
        // The figures are the overlay columns of the stored book, beside the book's tau.
        EXPECT_NE(src.find("trade_ngin::report_risk_metrics(\n"
                           "            overlay_columns.overlay_risk, portfolio_config.overlay_tau, "
                           "overlay_columns.var_95_1d,"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("std::cout << trade_ngin::report_risk_console(report_risk);"),
                  std::string::npos)
            << runner;
        EXPECT_NE(src.find("for (const auto& [name, value] : report_risk) {"), std::string::npos)
            << runner;
        // portfolio_var's stored value is as it was.
        EXPECT_NE(src.find("{\"portfolio_var\", portfolio_var},"), std::string::npos) << runner;
        EXPECT_NE(src.find("portfolio_var = r.portfolio_var;"), std::string::npos) << runner;
    }
    const std::string email = read_source("src/core/email_sender.cpp");
    if (email.empty()) GTEST_SKIP() << "email source not found";
    EXPECT_EQ(email.find("<strong>Portfolio VaR:</strong>"), std::string::npos);
    EXPECT_EQ(email.find("strategy_metrics.find(\"Portfolio VaR\")"), std::string::npos);
}
