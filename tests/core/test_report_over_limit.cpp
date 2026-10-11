// tests/core/test_report_over_limit.cpp
//
// T-8D R20 as LOOP_SPEC v6.2 restates it: a futures book's daily report prints the stored book's
// gross and net leverage on the sizing capital E_t beside the limits the overlay ran with (L_max,
// L_net_max) and the per-name cap L, and the day's marks from risk_detail
// (over_limit_after_rounding, over_limit_by_hold, CAP). A day with no mark says so; a day whose
// row carries no risk_detail says it has no reading and prints no figure. The two leverage lines
// the report already had are on the account value and now say so. The equity report is as it was.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/email_sender.hpp"
#include "trade_ngin/core/report_over_limit.hpp"

using namespace trade_ngin;

namespace {

const auto npos = std::string::npos;

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

// A book's portfolio modules: one carver module that carries the overlay's limits.
std::vector<RiskModuleConfig> modules_with(double l_max, double l_net_max) {
    CarverModuleConfig carver;
    carver.r_max = 2.25;
    carver.r_jump_max = 4.5;
    carver.r_shock_max = 4.0;
    carver.max_gross_leverage = l_max;
    carver.max_net_leverage = l_net_max;
    RiskModuleConfig module;
    module.id = "carver";
    module.type = "carver";
    module.params = carver;
    return {module};
}

// A sized rebalance the overlay answered, sized on E_t = 495,186.141684.
OnePassDay sized_day() {
    OnePassDay day;
    day.ran = true;
    day.sized = true;
    day.sizing_capital = 495186.141684;
    return day;
}

// The row's overlay columns: the stored book reads L = 1.6023570395 and L_net = 0.4118451282.
OverlayColumns columns_of(double gross, double net) {
    OverlayColumns columns;
    columns.overlay_gross_leverage = gross;
    columns.overlay_net_leverage = net;
    return columns;
}

int count_of(const std::string& text, const std::string& needle) {
    int n = 0;
    for (size_t at = text.find(needle); at != npos; at = text.find(needle, at + needle.size())) ++n;
    return n;
}

}  // namespace

TEST(ReportOverLimit, ADayWithNoMarkPrintsTheLeverageOnTheSizingCapitalAndSaysNoMark) {
    const OverLimitReport report = over_limit_report(
        sized_day(), columns_of(1.602357039519787, 0.4118451281904877), modules_with(8.0, 6.0), 2.0);
    EXPECT_TRUE(report.has_reading);
    EXPECT_DOUBLE_EQ(report.gross_leverage, 1.602357039519787);
    EXPECT_DOUBLE_EQ(report.net_leverage, 0.4118451281904877);
    EXPECT_EQ(over_limit_report_html(report),
              "<div class=\"metric\"><strong>Gross Leverage (on the sizing capital E_t):</strong> "
              "1.60x (limit L_max 8.00x)</div>\n"
              "<div class=\"metric\"><strong>Net Leverage (on the sizing capital E_t):</strong> "
              "0.41x (limit L_net_max 6.00x in either direction)</div>\n"
              "<div class=\"metric\"><strong>Per-name cap L:</strong> 2.00x of the sizing capital "
              "E_t</div>\n"
              "<div class=\"metric\"><strong>Over limit after rounding:</strong> no mark</div>\n"
              "<div class=\"metric\"><strong>Over limit by hold:</strong> no mark</div>\n"
              "<div class=\"metric\"><strong>CAP:</strong> no mark</div>\n");
}

TEST(ReportOverLimit, TheLimitsAreTheConfiguredValuesNeverALiteral) {
    // A book configured with other limits prints those; a short book prints its signed net.
    const OverLimitReport report =
        over_limit_report(sized_day(), columns_of(3.007578375, -1.161224375), modules_with(7.5, 5.5), 1.5);
    EXPECT_DOUBLE_EQ(report.gross_limit, 7.5);
    EXPECT_DOUBLE_EQ(report.net_limit, 5.5);
    EXPECT_DOUBLE_EQ(report.per_name_cap, 1.5);
    const std::string html = over_limit_report_html(report);
    EXPECT_NE(html.find("3.01x (limit L_max 7.50x)"), npos) << html;
    EXPECT_NE(html.find("-1.16x (limit L_net_max 5.50x in either direction)"), npos) << html;
    EXPECT_NE(html.find("<strong>Per-name cap L:</strong> 1.50x of the sizing capital E_t"), npos)
        << html;
    EXPECT_EQ(html.find("8.00x"), npos) << html;
    EXPECT_EQ(html.find("6.00x"), npos) << html;
    EXPECT_EQ(html.find("2.00x"), npos) << "the retired 2.0 limit, or a typed cap: " << html;
    // A module that carries no overlay limits (an old gate) gives none.
    std::vector<RiskModuleConfig> old_gate = modules_with(4.0, 2.0);
    std::get<CarverModuleConfig>(old_gate[0].params).r_max = 0.0;
    EXPECT_DOUBLE_EQ(over_limit_report(sized_day(), columns_of(1.0, 1.0), old_gate, 2.0).gross_limit,
                     0.0);
}

TEST(ReportOverLimit, ADayOverLimitAfterRoundingPrintsItsTermsAndExcess) {
    OnePassDay day = sized_day();
    day.over_limit_after_rounding_terms = "R_shock;L_g";
    day.over_limit_after_rounding_excess = 0.37125;
    const std::string html = over_limit_report_html(
        over_limit_report(day, columns_of(8.4312, 5.2), modules_with(8.0, 6.0), 2.0));
    EXPECT_NE(html.find("<strong>Gross Leverage (on the sizing capital E_t):</strong> 8.43x (limit "
                        "L_max 8.00x)"),
              npos)
        << html;
    EXPECT_NE(html.find("<div class=\"metric\"><strong>Over limit after rounding:</strong> terms "
                        "R_shock;L_g; excess 0.3713 (in units of the largest stored per-contract "
                        "weight)</div>\n"),
              npos)
        << html;
    EXPECT_NE(html.find("<strong>Over limit by hold:</strong> no mark"), npos) << html;
    EXPECT_NE(html.find("<strong>CAP:</strong> no mark"), npos) << html;
}

TEST(ReportOverLimit, ADayOverLimitByHoldWithCapPrintsTheTermsTheSymbolsAndTheCap) {
    OnePassDay day = sized_day();
    day.over_limit_by_hold_terms = "R_jump;CAP";
    day.over_limit_by_hold_symbols = "CL ZN";
    const std::string html = over_limit_report_html(
        over_limit_report(day, columns_of(2.5, -0.3), modules_with(8.0, 6.0), 2.0));
    EXPECT_NE(html.find("<strong>Over limit after rounding:</strong> no mark"), npos) << html;
    EXPECT_NE(html.find("<div class=\"metric\"><strong>Over limit by hold:</strong> terms "
                        "R_jump;CAP; symbols CL ZN</div>\n"),
              npos)
        << html;
    EXPECT_NE(html.find("<div class=\"metric\"><strong>CAP:</strong> marked: a held row is beyond "
                        "the per-name cap L 2.00x</div>\n"),
              npos)
        << html;
    // A hold mark that does not name CAP leaves the CAP line unmarked; CAP alone marks it.
    day.over_limit_by_hold_terms = "R_jump";
    EXPECT_NE(over_limit_report_html(
                  over_limit_report(day, columns_of(2.5, -0.3), modules_with(8.0, 6.0), 2.0))
                  .find("<strong>CAP:</strong> no mark"),
              npos);
    EXPECT_TRUE(over_limit_terms_name_cap("CAP"));
    EXPECT_TRUE(over_limit_terms_name_cap("L_g;CAP"));
    EXPECT_FALSE(over_limit_terms_name_cap("R;R_jump"));
    EXPECT_FALSE(over_limit_terms_name_cap(""));
}

TEST(ReportOverLimit, ARowWithNoRiskDetailSaysItHasNoReadingAndPrintsNoFigure) {
    const std::string expected =
        "<div class=\"metric\"><strong>Leverage on the sizing capital E_t and over-limit "
        "marks:</strong> no reading (no sized rebalance this run: the day's row carries no "
        "risk_detail)</div>\n";
    // A refused overlay: the pass ran on a cycle that trades and could not answer. Whatever the
    // record holds, nothing of it is printed.
    OnePassDay refused = sized_day();
    refused.refused = true;
    refused.over_limit_after_rounding_terms = "L_g";
    refused.over_limit_after_rounding_excess = 1.0;
    const OverLimitReport report =
        over_limit_report(refused, OverlayColumns{}, modules_with(8.0, 6.0), 2.0);
    EXPECT_FALSE(report.has_reading);
    EXPECT_EQ(over_limit_report_html(report), expected);
    // No pass at all (no session, a sizing hold, an all-JUNK rebalance).
    EXPECT_EQ(over_limit_report_html(
                  over_limit_report(OnePassDay{}, OverlayColumns{}, modules_with(8.0, 6.0), 2.0)),
              expected);
    // The default report (a caller that hands none) is the same line: never a zero.
    EXPECT_EQ(over_limit_report_html(OverLimitReport{}), expected);
    EXPECT_EQ(expected.find("0.00"), npos);
    EXPECT_EQ(expected.find("no mark"), npos) << "\"no reading\" is not \"no mark\"";
}

// The body both futures runners build (the per-strategy overload), with no database.
TEST(ReportOverLimit, TheFuturesReportNamesBothDenominatorsAndCarriesTheLines) {
    const std::map<std::string, double> metrics = {{"Gross Leverage", 1.5957404},
                                                   {"Net Leverage", 0.4101445},
                                                   {"Current Portfolio Value", 497239.3978}};
    OnePassDay day = sized_day();
    day.over_limit_by_hold_terms = "CAP";
    day.over_limit_by_hold_symbols = "ZN";
    const OverLimitReport report = over_limit_report(
        day, columns_of(1.602357039519787, 0.4118451281904877), modules_with(8.0, 6.0), 2.0);
    EmailSender sender{EmailSenderConfig{}};
    const std::string body = sender.generate_trading_report_body(
        StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt, metrics,
        StrategyExecutionsMap{}, "2026-04-24", "CONSERVATIVE_PORTFOLIO", true, {}, nullptr,
        StrategyPositionsMap{}, {}, {}, {}, "LIVE_TREND_FOLLOWING", report);
    // The two lines the report had, each with its denominator, then the new lines, in order.
    const std::string leverage =
        "<div class=\"metric\"><strong>Gross Leverage (on the account value V_t):</strong> "
        "<span>1.60x</span></div>\n"
        "<div class=\"metric\"><strong>Net Leverage (on the account value V_t):</strong> "
        "<span>0.41x</span></div>\n" +
        over_limit_report_html(report);
    EXPECT_NE(body.find(leverage), npos) << body;
    EXPECT_EQ(count_of(body, "<strong>Gross Leverage"), 2);
    EXPECT_EQ(count_of(body, "<strong>Net Leverage"), 2);
    EXPECT_EQ(body.find("<strong>Gross Leverage:</strong>"), npos) << "a leverage with no denominator";
    EXPECT_EQ(body.find("<strong>Net Leverage:</strong>"), npos);
    EXPECT_NE(body.find("<strong>Over limit by hold:</strong> terms CAP; symbols ZN"), npos);
    EXPECT_NE(body.find("<strong>CAP:</strong> marked"), npos);

    // A day whose row has no risk_detail: the account-value lines stay, the others say why not.
    const std::string held = sender.generate_trading_report_body(
        StrategyPositionsMap{}, std::unordered_map<std::string, Position>{}, std::nullopt, metrics,
        StrategyExecutionsMap{}, "2026-04-26", "CONSERVATIVE_PORTFOLIO", true, {}, nullptr,
        StrategyPositionsMap{}, {}, {}, {}, "LIVE_TREND_FOLLOWING",
        over_limit_report(OnePassDay{}, OverlayColumns{}, modules_with(8.0, 6.0), 2.0));
    EXPECT_NE(held.find("<strong>Gross Leverage (on the account value V_t):</strong> "
                        "<span>1.60x</span>"),
              npos);
    EXPECT_NE(held.find("<strong>Leverage on the sizing capital E_t and over-limit marks:</strong> "
                        "no reading"),
              npos)
        << held;
    EXPECT_EQ(held.find("(on the sizing capital E_t):"), npos) << "a figure with no reading";
    EXPECT_EQ(held.find("no mark"), npos);
}

// The equity report (the other overload) runs no overlay: its leverage section is as it was.
TEST(ReportOverLimit, TheEquityReportIsUnchanged) {
    const std::map<std::string, double> metrics = {{"Gross Leverage", 0.8123},
                                                   {"Net Leverage", 0.1042}};
    EmailSender sender{EmailSenderConfig{}};
    const std::string body = sender.generate_trading_report_body(
        std::unordered_map<std::string, Position>{}, std::nullopt, metrics,
        std::vector<ExecutionReport>{}, "2026-06-16");
    EXPECT_NE(body.find("<div class=\"metric\"><strong>Gross Leverage:</strong> "
                        "<span>0.81x</span></div>\n"
                        "<div class=\"metric\"><strong>Net Leverage:</strong> "
                        "<span>0.10x</span></div>\n<br>\n"),
              npos)
        << body;
    for (const char* absent : {"sizing capital E_t", "account value V_t", "Over limit", "L_max",
                               "<strong>CAP:"}) {
        EXPECT_EQ(body.find(absent), npos) << absent;
    }
}

// The runners are `main()`s: this guard READS SOURCE, to keep the two futures twins in step. The
// behaviour is executed by the tests above and by the gate's email bodies.
TEST(ReportOverLimitTwinSourceGuard, BothFuturesRunnersHandTheReportTheSameInputs) {
    for (const char* runner :
         {"apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_source(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src,
                           "trade_ngin::over_limit_report(one_pass_day, overlay_columns,\n"
                           "                                                      "
                           "portfolio_config.risk_modules,\n"
                           "                                                      "
                           "portfolio_config.per_name_cap),"),
                  1)
            << runner;
    }
    const std::string equity = read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (equity.empty()) GTEST_SKIP() << "equity runner source not found";
    EXPECT_EQ(equity.find("over_limit_report"), npos);
}
