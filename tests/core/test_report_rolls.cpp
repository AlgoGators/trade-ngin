// tests/core/test_report_rolls.cpp
//
// Rolls step 4 (R4b; rulings 10 and 18): the report's roll pieces with per_contract_rolls on
// (core/report_rolls.hpp), and the outlook the runner reads for a held root (report_roll_view.hpp):
//   * the roll line reproduces the worked example's (silver, December to March, 2025-11-25) to the
//     character, and words a short, a backwardated gap and a roll whose names were not found;
//   * the Rollover Warning box is the 2026-10-13 design (the red box, "Currently holding X -> Switch to
//     Y") with the latest run date and the deadline, lists only held contracts the rule rolls whose
//     window is open, and is absent otherwise;
//   * the outlook of a built series gives the held contract, the window, the last actionable bar's run
//     date, the deadline and the cycle front (the GC fixture of test_contract_rolls_api.cpp);
//   * the Front Month and Contract Months cells, the decimals of a tick, the month names.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "../data/contract_rolls_test_util.hpp"
#include "trade_ngin/core/report_rolls.hpp"
#include "trade_ngin/data/report_roll_view.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

namespace {

ExecutionReport closing_leg(double move, double points, double value, double cost) {
    ExecutionReport e;
    e.symbol = "SI.v.0";
    e.execution_type = ExecutionType::ROLL;
    e.exec_id = "EXEC_SI.v.0_20251125_RC";
    e.total_transaction_costs = Decimal(cost);
    e.roll_bar_move = move;
    e.roll_gap_points = points;
    e.roll_gap_value = value;
    return e;
}

ReportRollOutlook outlook(const std::string& root, const std::string& held, bool in_window, bool rule_rolls = true) {
    ReportRollOutlook o;
    o.root = root;
    o.held = held;
    o.cycle = "HKNUZ";
    o.rule_rolls = rule_rolls;
    o.in_window = in_window;
    o.roll_by = "2025-11-25";
    o.deadline = "2025-11-28";
    o.first_notice = true;
    o.target = "SIH6";
    return o;
}

}  // namespace

TEST(ReportRolls, TheRollLineIsTheWorkedExamples) {
    // The closing leg's split and both legs' costs (22.62 + 22.67), as the run stored them.
    const auto leg = closing_leg(50.960 * 5000.0 - 51.175 * 5000.0, 51.630 - 50.960, (51.630 - 50.960) * 5000.0,
                                 22.6204399);
    const ReportRollNames names{"Silver", "December", "March", report_tick_places(0.005)};
    EXPECT_EQ(report_roll_line(names, "SI.v.0", leg, 22.6204399 + 22.67119529),
              "Silver, December to March: market move booked -$1,075; contract price difference 0.670 "
              "($3,350, not profit or loss); roll cost $45.29.");
}

TEST(ReportRolls, TheRollLineWordsAShortABackwardatedGapAndUnknownNames) {
    // Short 2 contracts of a Treasury, the new contract cheaper by 0.5 (a long would receive it).
    const auto leg = closing_leg(2343.75, -0.5, 1000.0, 5.0);
    const ReportRollNames names{"10-Year-T-Note", "June", "September", report_tick_places(0.015625)};
    EXPECT_EQ(report_roll_line(names, "ZN.v.0", leg, 10.0),
              "10-Year-T-Note, June to September: market move booked $2,344; contract price difference "
              "-0.500000 ($1,000, not profit or loss); roll cost $10.00.");
    EXPECT_EQ(report_roll_line(ReportRollNames{}, "ZN.v.0", leg, 10.0),
              "ZN.v.0: market move booked $2,344; contract price difference -0.5000 ($1,000, not profit or "
              "loss); roll cost $10.00.");
    ExecutionReport no_split = leg;
    no_split.roll_bar_move.reset();
    EXPECT_EQ(report_roll_line(names, "ZN.v.0", no_split, 10.0), "") << "a leg with no split has no line";
}

TEST(ReportRolls, TheRolloverWarningIsTheOldBoxWithTheRunDateAndTheDeadline) {
    ReportRolls rolls;
    rolls.per_contract_rolls = true;
    rolls.outlook["SI"] = outlook("SI", "SIZ5", true);
    rolls.outlook["GC"] = outlook("GC", "GCZ5", false);           // window not open
    rolls.outlook["MES"] = outlook("MES", "MESZ5", true, false);  // the vendor's series: not the rule's
    auto ltd = outlook("6L", "6LX5", true);
    ltd.first_notice = false;
    ltd.deadline = "2025-11-28";
    ltd.target = "6LZ5";
    rolls.outlook["6L"] = ltd;
    const std::string box = report_rollover_warning_html(rolls);
    EXPECT_NE(box.find("<div style=\"background-color: #fff5f5; border-left: 4px solid #dc2626;"), std::string::npos);
    EXPECT_NE(box.find("<strong>Rollover Notice:</strong>"), std::string::npos);
    EXPECT_NE(box.find("<li><strong>SI:</strong> Currently holding <strong>SIZ5</strong> \xE2\x86\x92 Switch to "
                       "<strong>SIH6</strong> by 11-25, first notice 11-28</li>"),
              std::string::npos)
        << box;
    EXPECT_NE(box.find("<li><strong>6L:</strong> Currently holding <strong>6LX5</strong> \xE2\x86\x92 Switch to "
                       "<strong>6LZ5</strong> by 11-25, last trading day 11-28</li>"),
              std::string::npos)
        << box;
    EXPECT_LT(box.find("6L:"), box.find("SI:")) << "root order";
    EXPECT_EQ(box.find("GCZ5"), std::string::npos) << "outside its window";
    EXPECT_EQ(box.find("MESZ5"), std::string::npos) << "not rolled by the rule";
    rolls.outlook.erase("SI");
    rolls.outlook.erase("6L");
    EXPECT_EQ(report_rollover_warning_html(rolls), "") << "no held contract inside its window: no box";
    EXPECT_EQ(report_rollover_warning_html(ReportRolls{}), "");
}

TEST(ReportRolls, TheOutlookOfABuiltSeriesNamesTheWindowTheRunDateTheDeadlineAndTheFront) {
    // test_contract_rolls_api.cpp's GC fixture: GCQ6 held; first notice Fri 2026-07-31, window from
    // 07-24, last actionable bar Wed 07-29 (its run is dated 07-30), GCZ6 the cycle front.
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (const char* dt : {"2026-07-22", "2026-07-23", "2026-07-24", "2026-07-27"}) {
        rows.push_back(contract_rolls_test::bar("GC", "GCQ6", "q", dt, "2026-08-27", 3300, 80000));
        rows.push_back(contract_rolls_test::bar("GC", "GCZ6", "z", dt, "2026-12-29", 3350, 9000));
        VendorBar v;
        v.date = d(dt);
        v.symbol = "GC.v.0";
        v.instrument_id = "q";
        grid.push_back(v);
    }
    auto book = std::make_shared<ContractBook>(ContractBook::build(rows).value());
    const TradingCalendar cal = contract_rolls_test::shipped_calendar();
    RuleInputs in;
    in.root = "GC";
    in.grid = grid;
    in.book = book.get();
    in.cal = &cal;
    in.cycle = "GJMQZ";
    auto held = apply_roll_rule(in);
    ASSERT_TRUE(held.is_ok()) << held.error()->what();
    const ContractRollSeries series(held.value(), book, "GJMQZ", std::nullopt, cal);
    auto before = report_outlook_of(series, "GC", "GJMQZ", d("2026-07-23"));
    ASSERT_TRUE(before.is_ok()) << before.error()->what();
    EXPECT_EQ(before.value().held, "GCQ6");
    EXPECT_FALSE(before.value().in_window) << "the day before the window's first day";
    auto o = report_outlook_of(series, "GC", "GJMQZ", d("2026-07-24"));
    ASSERT_TRUE(o.is_ok());
    EXPECT_TRUE(o.value().in_window);
    EXPECT_TRUE(o.value().rule_rolls);
    EXPECT_EQ(o.value().roll_by, "2026-07-30");
    EXPECT_EQ(o.value().deadline, "2026-07-31");
    EXPECT_TRUE(o.value().first_notice);
    EXPECT_EQ(o.value().target, "GCZ6");
    EXPECT_EQ(o.value().cycle, "GJMQZ");
    EXPECT_TRUE(report_outlook_of(series, "GC", "GJMQZ", d("2026-07-01")).is_error()) << "before the series";
}

TEST(ReportRolls, TheSymbolsReferenceCellsTheMonthsAndTheTicks) {
    ReportRolls rolls;
    rolls.per_contract_rolls = true;
    rolls.outlook["SI"] = outlook("SI", "SIZ5", false);
    EXPECT_EQ(report_front_month(rolls, "SI"), "SIZ5");
    EXPECT_EQ(report_front_month(rolls, "GC"), "");
    EXPECT_EQ(report_contract_months(rolls, "SI", "JAN, MAR, MAY, JULY, SEP, DEC"), "Mar May Jul Sep Dec");
    EXPECT_EQ(report_contract_months(rolls, "GC", "FEB, APR"), "FEB, APR") << "no outlook: the metadata's";
    EXPECT_EQ(report_cycle_months("FGHJKMNQUVXZ"),
              "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec");
    EXPECT_EQ(report_contract_month("SI", "SIZ5"), 12u);
    EXPECT_EQ(report_contract_month("6L", "6LF6"), 1u);
    EXPECT_EQ(report_contract_month("SI", "GCZ5"), 0u);
    EXPECT_EQ(report_month_name(3), "March");
    EXPECT_EQ(report_month_name(13), "");
    EXPECT_EQ(report_tick_places(0.005), 3);
    EXPECT_EQ(report_tick_places(0.015625), 6);
    EXPECT_EQ(report_tick_places(0.00005), 5);
    EXPECT_EQ(report_tick_places(0.25), 2);
    EXPECT_EQ(report_tick_places(5.0), 0);
    EXPECT_EQ(report_tick_places(0.0), 4);
    EXPECT_EQ(report_whole_dollars(-1075.4), "-$1,075");
    EXPECT_EQ(report_whole_dollars(1234567.6), "$1,234,568");
    EXPECT_EQ(report_whole_dollars(-0.4), "$0");
    EXPECT_EQ(report_cents(45.2916), "$45.29");
    EXPECT_EQ(report_cents(-1234.5), "-$1,234.50");
}

// The view from its sources, no database (report_roll_view's pure core): a root the rule rolls gets its
// outlook from its series; a micro the vendor's contract on its latest grid bar and no window; a root with
// no cycle, a series that fails and a read that throws get no outlook; each roll's months come from its two
// contracts by id, the name is the metadata's trimmed, the decimals the tick's; a roll whose contracts
// cannot be read names no months; names that cannot be read leave the symbol; a settled day that is not a
// date gives an empty view, still marked as the switch on.
TEST(ReportRolls, TheViewIsBuiltFromItsSources) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (const char* dt : {"2026-07-22", "2026-07-23", "2026-07-24", "2026-07-27"}) {
        rows.push_back(contract_rolls_test::bar("GC", "GCQ6", "q", dt, "2026-08-27", 3300, 80000));
        rows.push_back(contract_rolls_test::bar("GC", "GCZ6", "z", dt, "2026-12-29", 3350, 9000));
        VendorBar v;
        v.date = d(dt);
        v.symbol = "GC.v.0";
        v.instrument_id = "q";
        grid.push_back(v);
    }
    auto book = std::make_shared<ContractBook>(ContractBook::build(rows).value());
    const TradingCalendar cal = contract_rolls_test::shipped_calendar();
    RuleInputs in;
    in.root = "GC";
    in.grid = grid;
    in.book = book.get();
    in.cal = &cal;
    in.cycle = "GJMQZ";
    const ContractRollSeries series(apply_roll_rule(in).value(), book, "GJMQZ", std::nullopt, cal);
    const auto micro_book = ContractBook::build(
        {contract_rolls_test::bar("MES", "MESU6", "m1", "2026-07-24", "2026-09-18", 6400, 100)}).value();

    ReportRollSources src;
    src.series = [&](const std::string& root, Day) -> Result<ContractRollSeries> {
        if (root == "GC") return series;
        return make_error<ContractRollSeries>(ErrorCode::DATA_NOT_FOUND, "no contract bars", "test");
    };
    src.vendor_grid = [&](const std::string& root, Day, Day) {
        VendorBar v;
        v.date = d("2026-07-24");
        v.symbol = root + ".v.0";
        v.instrument_id = "m1";
        return std::vector<VendorBar>{v};
    };
    src.contracts_around = [&](const std::string& root, Day) -> Result<ContractBook> {
        if (root == "GC") return *book;
        if (root == "MES") return micro_book;
        throw std::runtime_error("unreadable");
    };
    src.names = [] { return std::map<std::string, std::string>{{"GC", "Gold "}}; };

    ReportRollQuery q;
    q.date = "2026-07-24";
    q.held_symbols = {"GC.v.0", "MES.v.0", "ZN.v.0", "XX.v.0"};
    q.rolls = {{"EXEC_GC.v.0_20260727_RC", "GC.v.0", "q", "z", "2026-07-27"},
               {"EXEC_ZN.v.0_20260727_RC", "ZN.v.0", "a", "b", "2026-07-27"}};
    const auto view = report_roll_view(q, src, [](const std::string& s) { return s == "GC.v.0" ? 0.1 : 0.0; });
    EXPECT_TRUE(view.per_contract_rolls);
    ASSERT_EQ(view.outlook.count("GC"), 1u);
    EXPECT_EQ(view.outlook.at("GC").held, "GCQ6");
    EXPECT_TRUE(view.outlook.at("GC").in_window);
    EXPECT_EQ(view.outlook.at("GC").roll_by, "2026-07-30");
    ASSERT_EQ(view.outlook.count("MES"), 1u);
    EXPECT_EQ(view.outlook.at("MES").held, "MESU6");
    EXPECT_FALSE(view.outlook.at("MES").rule_rolls);
    EXPECT_EQ(view.outlook.at("MES").cycle, "HMUZ");
    EXPECT_EQ(view.outlook.count("ZN"), 0u) << "its series failed";
    EXPECT_EQ(view.outlook.count("XX"), 0u) << "no cycle";
    const auto& gc = view.names.at("EXEC_GC.v.0_20260727_RC");
    EXPECT_EQ(gc.name, "Gold");
    EXPECT_EQ(gc.from_month, "August");
    EXPECT_EQ(gc.to_month, "December");
    EXPECT_EQ(gc.gap_places, 1);
    const auto& zn = view.names.at("EXEC_ZN.v.0_20260727_RC");
    EXPECT_EQ(zn.name, "");
    EXPECT_EQ(zn.from_month, "") << "its contracts could not be read";
    EXPECT_EQ(zn.gap_places, 4);

    ReportRollSources broken = src;
    broken.names = []() -> std::map<std::string, std::string> { throw std::runtime_error("no metadata"); };
    EXPECT_EQ(report_roll_view(q, broken, {}).names.at("EXEC_GC.v.0_20260727_RC").name, "");
    ReportRollQuery none = q;
    none.rolls.clear();
    EXPECT_TRUE(report_roll_view(none, src, {}).names.empty());
    ReportRollQuery bad = q;
    bad.date = "24 July";
    const auto empty = report_roll_view(bad, src, {});
    EXPECT_TRUE(empty.per_contract_rolls);
    EXPECT_TRUE(empty.outlook.empty());
    EXPECT_TRUE(empty.names.empty());
}
