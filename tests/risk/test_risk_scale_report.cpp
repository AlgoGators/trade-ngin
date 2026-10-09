// RISK_SCALE_REPORT (RA-01, T-7b-1 commit C7): the runner-side line that puts the stored
// reporter's risk_scale beside the scale the PortfolioManager actually applied, built from a
// synthetic PortfolioManager::last_risk_decisions() record. No PortfolioManager is run here: the
// summary is a pure function of the record, and these cases pin each documented field.

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

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
