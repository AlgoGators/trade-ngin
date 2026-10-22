// RISK_SCALE_REPORT (RA-01, T-7b-1 commit C7): the runner-side line that puts the stored
// reporter's risk_scale beside the scale the PortfolioManager actually applied, built from a
// synthetic PortfolioManager::last_risk_decisions() record. No PortfolioManager is run here: the
// summary is a pure function of the record, and these cases pin each documented field.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/risk/risk_module.hpp"
#include "trade_ngin/risk/risk_scale_report.hpp"

using namespace trade_ngin;

namespace {

// One record row the way PortfolioManager::record_risk_decision writes it.
RiskDecisionRecord row(RiskPhase phase, int lap, RiskScope scope, const std::string& scope_id,
                       const std::string& module_id, RiskAction applied, double factor,
                       bool empty_book = false) {
    RiskDecisionRecord r;
    r.phase = phase;
    r.lap = lap;
    r.scope = scope;
    r.scope_id = scope_id;
    r.module_id = module_id;
    r.requested.module_id = module_id;
    if (applied == RiskAction::SCALE) {
        r.requested.action = RiskAction::SCALE;
        r.requested.scale = factor;
    }
    r.applied_action = applied;
    r.applied_factor = Decimal(factor);
    r.empty_book = empty_book;
    return r;
}

RiskDecisionRecord lap_row(int lap, const std::string& module_id, RiskAction applied,
                           double factor = 1.0, bool empty_book = false) {
    return row(RiskPhase::LAP, lap, RiskScope::PORTFOLIO, "PORTFOLIO_MANAGER", module_id, applied,
               factor, empty_book);
}

// The value the line prints for applied_cumulative must read back as the very double the
// summary holds (%.17g round-trips a double).
double read_field(const std::string& line, const std::string& key) {
    const auto at = line.find(" " + key + "=");
    EXPECT_NE(at, std::string::npos) << key << " missing from: " << line;
    return std::strtod(line.c_str() + at + key.size() + 2, nullptr);
}

// A runner's source, found by walking up from the test's working directory; empty when absent.
std::string read_runner(const std::string& relative) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
        const fs::path candidate = dir / relative;
        if (fs::exists(candidate, ec)) {
            std::ifstream in(candidate);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }
        if (dir == dir.parent_path()) break;
    }
    return std::string();
}

std::size_t count_of(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

Position stored_row(const std::string& symbol, double quantity) {
    Position p;
    p.symbol = symbol;
    p.quantity = Quantity(quantity);
    return p;
}

}  // namespace

// Several laps, one cutting: the Carver module cuts on lap 1, nothing cuts on lap 2. A second
// portfolio module (warn) records NONE rows on both laps and is not a cut. A sleeve SCALE and the
// post-rounding row are outside the definition and must not move any field.
TEST(RiskScaleReportTest, TwoLapsOneCutNamesTheCuttingModule) {
    std::vector<RiskDecisionRecord> rec;
    rec.push_back(row(RiskPhase::SLEEVE, 0, RiskScope::SLEEVE, "TREND_FOLLOWING", "sleeve_cut",
                      RiskAction::SCALE, 0.5));
    rec.push_back(lap_row(1, "carver", RiskAction::SCALE, 0.85));
    rec.push_back(lap_row(1, "warn_gross", RiskAction::NONE));
    rec.push_back(lap_row(2, "carver", RiskAction::NONE));
    rec.push_back(lap_row(2, "warn_gross", RiskAction::NONE));
    rec.push_back(row(RiskPhase::POST_ROUNDING, 2, RiskScope::PORTFOLIO, "PORTFOLIO_MANAGER",
                      "carver", RiskAction::NONE, 1.0));

    const RiskScaleSummary s = summarize_applied_risk(rec);
    EXPECT_EQ(s.applied_cumulative, static_cast<double>(Decimal(0.85)));
    EXPECT_EQ(s.laps, 2);
    EXPECT_EQ(s.cutting_laps, 1);
    EXPECT_EQ(s.binding_module, "carver");

    EXPECT_EQ(format_risk_scale_report(0.88163, s),
              "RISK_SCALE_REPORT reporter=0.88163000000000002 "
              "applied_cumulative=0.84999999999999998 laps=2 cutting_laps=1 "
              "binding_module=carver");
}

// Two cutting laps by two different modules: the cumulative is the left-to-right product of the
// QUANTISED factors, bit for bit what the PortfolioManager accumulates into RiskContext::applied
// (and prints as RISK_APPLIED cumulative=); the binding module is the LAST cut's.
TEST(RiskScaleReportTest, TwoCutsMultiplyTheQuantisedFactorsAndTheLastCutBinds) {
    std::vector<RiskDecisionRecord> rec;
    rec.push_back(lap_row(1, "carver", RiskAction::SCALE, 0.8500051234));
    rec.push_back(lap_row(1, "halve", RiskAction::NONE));
    rec.push_back(lap_row(2, "carver", RiskAction::NONE));
    rec.push_back(lap_row(2, "halve", RiskAction::SCALE, 0.9));
    rec.push_back(lap_row(3, "carver", RiskAction::NONE));
    rec.push_back(lap_row(3, "halve", RiskAction::NONE));

    double expected = 1.0;
    expected *= static_cast<double>(Decimal(0.8500051234));
    expected *= static_cast<double>(Decimal(0.9));

    const RiskScaleSummary s = summarize_applied_risk(rec);
    EXPECT_EQ(s.applied_cumulative, expected);
    EXPECT_EQ(s.laps, 3);
    EXPECT_EQ(s.cutting_laps, 2);
    EXPECT_EQ(s.binding_module, "halve");

    const std::string line = format_risk_scale_report(0.9, s);
    EXPECT_EQ(read_field(line, "applied_cumulative"), expected);
    EXPECT_EQ(read_field(line, "reporter"), 0.9);
    EXPECT_NE(line.find(" laps=3 cutting_laps=2 binding_module=halve"), std::string::npos) << line;
}

// The EQUITY_MR book runs the `none` module: the PortfolioManager holds no module, records no
// row, and the line says so from the empty record (no hard-coded "none book" branch). The same
// empty record is what a futures day with no process_market_data call leaves: a zero-lap day.
TEST(RiskScaleReportTest, ANoneBookOrAZeroLapDayReadsOneZeroZeroNone) {
    const RiskScaleSummary s = summarize_applied_risk({});
    EXPECT_EQ(s.applied_cumulative, 1.0);
    EXPECT_EQ(s.laps, 0);
    EXPECT_EQ(s.cutting_laps, 0);
    EXPECT_EQ(s.binding_module, "none");
    EXPECT_EQ(format_risk_scale_report(0.8082, s),
              "RISK_SCALE_REPORT reporter=0.80820000000000003 applied_cumulative=1 laps=0 "
              "cutting_laps=0 binding_module=none");
}

// A lap on an empty book ran (on_bars, one empty_book row per module) and counts as a lap; a
// SCALE whose quantised factor is exactly 1 multiplied by 1 and is not a cut; the risk step's
// own REFUSE row counts as a lap and is not a cut either.
TEST(RiskScaleReportTest, EmptyBookLapsFactorOneAndARefusalAreLapsButNotCuts) {
    std::vector<RiskDecisionRecord> rec;
    rec.push_back(lap_row(1, "carver", RiskAction::NONE, 1.0, /*empty_book=*/true));
    rec.push_back(lap_row(2, "carver", RiskAction::SCALE, 0.999999999));  // Decimal -> 1.0
    rec.push_back(lap_row(3, kRiskStepModuleId, RiskAction::REFUSE));

    const RiskScaleSummary s = summarize_applied_risk(rec);
    EXPECT_EQ(s.applied_cumulative, 1.0);
    EXPECT_EQ(s.laps, 3);
    EXPECT_EQ(s.cutting_laps, 0);
    EXPECT_EQ(s.binding_module, "none");
}

// The futures backtest has no snapshot reporter and stores no risk figure: reporter=na, and the
// rebalance date is appended because a backtest log line carries only the wall clock.
TEST(RiskScaleReportTest, TheBacktestLineSaysNaAndCarriesTheRebalanceDate) {
    std::vector<RiskDecisionRecord> rec;
    rec.push_back(lap_row(1, "carver", RiskAction::SCALE, 0.85));
    rec.push_back(lap_row(2, "carver", RiskAction::NONE));
    EXPECT_EQ(format_risk_scale_report(std::string("na"), summarize_applied_risk(rec),
                                       "2025-03-14"),
              "RISK_SCALE_REPORT reporter=na applied_cumulative=0.84999999999999998 laps=2 "
              "cutting_laps=1 binding_module=carver date=2025-03-14");
}

// ---- T-8c (1): the equity row's risk_scale, the equity RISK_DELIVERED book, the console lines ----

// T-8D R14: a book that runs no portfolio-risk overlay stores 1.0, whatever the snapshot reporter
// recommends (the reporter's 0.895 of the equity run dated 2026-06-16 is not a stored figure).
TEST(RiskScaleNoOverlayTest, TheStoredValueIsOneAndTheConsoleSaysWhatOneMeans) {
    EXPECT_EQ(kRiskScaleNoOverlay, 1.0);
    EXPECT_EQ(risk_scale_console_no_overlay(),
              "Risk Scale (risk_scale: 1.0 = no portfolio-risk overlay ran this run): 1.0000\n");
    // No request is printed: the book has none.
    EXPECT_EQ(risk_scale_console_no_overlay().find("risk_requested"), std::string::npos);
    EXPECT_EQ(risk_scale_console_no_overlay().find("N/A"), std::string::npos);
}

// T-8D-2 R68 on a futures row: the delivered scale and the request, each named, four decimals.
// CONSERVATIVE 2026-04-24: stored gross weight 1.60235704 over the capped target's 2.915205069
// (held rows in both), m_t = 1.
TEST(RiskScaleConsoleTest, PrintsTheDeliveredScaleAndTheRequestEachLabelled) {
    EXPECT_EQ(risk_scale_console(1.60235704 / 2.915205069, true, 1.0),
              "Risk Scale (risk_scale: delivered, stored gross / capped target gross): 0.5497\n"
              "Risk Requested (risk_requested: the overlay's m_t): 1.0000\n");
    // A cut day: the two differ and neither is the other's label.
    EXPECT_EQ(risk_scale_console(0.8612, true, 0.7305),
              "Risk Scale (risk_scale: delivered, stored gross / capped target gross): 0.8612\n"
              "Risk Requested (risk_requested: the overlay's m_t): 0.7305\n");
    // The delivered scale can exceed 1 (LOOP_SPEC section 7.2).
    EXPECT_NE(risk_scale_console(1.25, true, 1.0).find("): 1.2500\n"), std::string::npos);
}

// A row that stores no risk_detail (no session, a sizing hold, a refused overlay) has no request:
// the default m of an OnePassDay that did not run (1.0) is not printed as if the overlay said it.
TEST(RiskScaleConsoleTest, ARowWithNoRiskDetailPrintsNoRequest) {
    EXPECT_EQ(risk_scale_console(1.0, false, 1.0),
              "Risk Scale (risk_scale: delivered, stored gross / capped target gross): 1.0000\n"
              "Risk Requested (risk_requested: the overlay's m_t): N/A (no sized rebalance this "
              "run)\n");
    EXPECT_EQ(risk_scale_console(1.0, false, 0.5), risk_scale_console(1.0, false, 1.0));
}

// T-8D-2 R69: the equity runner's stored rows, one sleeve, give the book account_book_of gives for
// the same rows filed under one sleeve; a closed row (quantity 0) is in the book and adds nothing.
TEST(AccountBookOfRowsTest, AFlatListOfStoredRowsIsTheOneSleeveAccountBook) {
    const std::vector<Position> rows = {stored_row("AAPL", 12.5), stored_row("TMUS", 0.0),
                                        stored_row("MSFT", -3.0)};
    const auto book = account_book_of_rows(rows);
    ASSERT_EQ(book.size(), 3u);
    EXPECT_EQ(book.at("AAPL"), 12.5);
    EXPECT_EQ(book.at("TMUS"), 0.0);
    EXPECT_EQ(book.at("MSFT"), -3.0);

    std::unordered_map<std::string, std::unordered_map<std::string, Position>> one_sleeve;
    for (const auto& row : rows) one_sleeve["EQUITY_MR"][row.symbol] = row;
    EXPECT_EQ(book, account_book_of(one_sleeve));
    EXPECT_TRUE(account_book_of_rows(std::vector<Position>{}).empty());
}

// The line the equity runner prints once it passes the stored rows: final_gross is the stored
// book's, not the book the strategy asked for. Target 10 AAPL and 4 MSFT (1,000 + 800); the runner
// stores 10 AAPL and a closed MSFT row.
TEST(AccountBookOfRowsTest, TheDeliveredLineMeasuresTheStoredRows) {
    const std::map<std::string, double> target = {{"AAPL", 10.0}, {"MSFT", 4.0}};
    const std::map<std::string, double> npc = {{"AAPL", 100.0}, {"MSFT", 200.0}};
    const std::vector<Position> stored = {stored_row("AAPL", 10.0), stored_row("MSFT", 0.0)};
    const DeliveredCut d = measure_delivered_cut(&target, account_book_of_rows(stored), npc);
    EXPECT_EQ(d.lap1_gross, 1800.0);
    EXPECT_EQ(d.final_gross, 1000.0);
    EXPECT_EQ(format_risk_delivered(RiskScaleSummary{}, d),
              "RISK_DELIVERED requested=1 delivered=0.55555555555555558 final_gross=1000 "
              "lap1_gross=1800 unpriced=0");
}

// READS SOURCE (a twin and wiring guard, not the test of a behaviour): the runners are `main()`s.
// The behaviours themselves are executed above; the stored cell and the log line are proven by the
// gate run of the commit (eq2: trading.live_results.risk_scale and the RISK_DELIVERED line).
TEST(RiskScaleRunnerWiringSourceGuard, TheEquityRunnerStoresOneAndMeasuresItsStoredRows) {
    const std::string src = read_runner("apps/strategies/live_equity_mean_reversion.cpp");
    if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
    EXPECT_EQ(src.find("risk_scale = r.recommended_scale"), std::string::npos)
        << "the equity runner still stores the reporter's recommended scale as risk_scale";
    EXPECT_EQ(count_of(src, "const double risk_scale = trade_ngin::kRiskScaleNoOverlay;"), 1u);
    EXPECT_EQ(count_of(src, "{\"risk_scale\", risk_scale},"), 1u);
    EXPECT_EQ(src.find("portfolio->last_delivered_cut()"), std::string::npos)
        << "RISK_DELIVERED still reads the PortfolioManager's own final book";
    EXPECT_EQ(count_of(src, "portfolio->delivered_cut_for_book(\n"
                            "                trade_ngin::account_book_of_rows(positions_to_save))"),
              1u);
    EXPECT_EQ(count_of(src, "std::cout << trade_ngin::risk_scale_console_no_overlay();"), 1u);
    EXPECT_EQ(src.find("\"Risk Scale: "), std::string::npos) << "an unlabelled Risk Scale line";
    // The reporter and its three columns stay.
    EXPECT_NE(src.find("trade_ngin::RiskManager snapshot_rm(risk_config);"), std::string::npos);
    for (const char* kept : {"portfolio_var = r.portfolio_var;", "max_correlation = r.correlation_risk;",
                             "jump_risk = r.jump_risk;", "{\"portfolio_var\", portfolio_var},",
                             "{\"max_correlation\", max_correlation},", "{\"jump_risk\", jump_risk},"}) {
        EXPECT_NE(src.find(kept), std::string::npos) << kept;
    }
}

// READS SOURCE (a twin guard): both futures runners print the same two labelled lines from the
// book they store, and neither prints the reporter's scale under the name Risk Scale.
TEST(RiskScaleRunnerWiringSourceGuard, TheFuturesTwinsPrintTheSameLabelledLines) {
    const std::string block =
        "        std::cout << trade_ngin::risk_scale_console(\n"
        "            overlay_answered ? portfolio->delivered_scale_for_book(\n"
        "                                   trade_ngin::account_book_of(strategy_positions_map))\n"
        "                             : 1.0,\n"
        "            overlay_answered, one_pass_day.risk_requested);\n";
    for (const char* runner :
         {"apps/strategies/live_portfolio_conservative.cpp", "apps/strategies/live_portfolio.cpp"}) {
        const std::string src = read_runner(runner);
        if (src.empty()) GTEST_SKIP() << "runner source not found from the test working directory";
        EXPECT_EQ(count_of(src, block), 1u) << runner;
        EXPECT_EQ(src.find("\"Risk Scale: "), std::string::npos) << runner;
        EXPECT_EQ(src.find("<< r.recommended_scale"), std::string::npos) << runner;
        // After the STRICT step, so the book read is the one the runner stores, and the reporter
        // with its three columns stays.
        const auto strict = src.find("std::vector<std::string> strict_rolled_back;");
        ASSERT_NE(strict, std::string::npos) << runner;
        EXPECT_LT(strict, src.find(block)) << runner;
        EXPECT_NE(src.find("trade_ngin::RiskManager snapshot_rm(snapshot_risk_config);"),
                  std::string::npos)
            << runner;
        for (const char* kept : {"{\"portfolio_var\", portfolio_var},",
                                 "{\"max_correlation\", max_correlation},",
                                 "{\"jump_risk\", jump_risk},"}) {
            EXPECT_NE(src.find(kept), std::string::npos) << runner << " " << kept;
        }
    }
}
