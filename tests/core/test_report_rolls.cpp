// tests/core/test_report_rolls.cpp
//
// The report's roll pieces with per_contract_rolls on
// (core/report_rolls.hpp), and the outlook the runner reads for a held root (report_roll_view.hpp):
//   * the roll line reproduces the worked example's (silver, December to March, 2025-11-25) to the
//     character, and words a short, a backwardated gap and a roll whose names were not found;
//   * the Rollover Warning box (the red box, "Currently holding X -> Switch to Y") carries the
//     latest run date and the deadline, lists only held contracts the rule rolls whose
//     window is open, and is absent otherwise;
//   * the outlook of a built series gives the held contract, the window, the last actionable bar's run
//     date, the deadline and the cycle front (the GC fixture of test_contract_rolls_api.cpp);
//   * the Front Month and Contract Months cells, the decimals of a tick, the month names.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../data/contract_rolls_test_util.hpp"
#include "trade_ngin/core/report_rolls.hpp"
#include "trade_ngin/core/report_settled_marks.hpp"
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

ReportRollOutlook outlook(const std::string& root, const std::string& held, bool in_window) {
    ReportRollOutlook o;
    o.root = root;
    o.held = held;
    o.cycle = "HKNUZ";
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
    // Short 2 contracts of a Treasury, the new contract cheaper by 0.5: the difference prints without a
    // sign (it is not profit or loss); the stored values keep theirs.
    const auto leg = closing_leg(2343.75, -0.5, 1000.0, 5.0);
    const ReportRollNames names{"10-Year-T-Note", "June", "September", report_tick_places(0.015625)};
    EXPECT_EQ(report_roll_line(names, "ZN.v.0", leg, 10.0),
              "10-Year-T-Note, June to September: market move booked $2,344; contract price difference "
              "0.500000 ($1,000, not profit or loss); roll cost $10.00.");
    EXPECT_EQ(report_roll_line(ReportRollNames{}, "ZN.v.0", leg, 10.0),
              "ZN.v.0: market move booked $2,344; contract price difference 0.5000 ($1,000, not profit or "
              "loss); roll cost $10.00.");
    // A short in contango: positive points, a negative stored value, printed as $1,420.
    const auto ng = closing_leg(-2040.0, 0.142, -1420.0, 31.69);
    EXPECT_EQ(report_roll_line(ReportRollNames{"Natural Gas Futures", "October", "November", 3}, "NG.v.0", ng, 31.69),
              "Natural Gas Futures, October to November: market move booked -$2,040; contract price difference "
              "0.142 ($1,420, not profit or loss); roll cost $31.69.");
    // A leg with no split gets no line (no root is on the vendor's series, so there is no
    // cost-only line); a micro's roll is worded like any other.
    ExecutionReport no_split = leg;
    no_split.roll_bar_move.reset();
    no_split.roll_gap_points.reset();
    no_split.roll_gap_value.reset();
    EXPECT_EQ(report_roll_line(ReportRollNames{"Micro E-Mini Russell-2000 Index", "September", "December", 1},
                               "M2K.v.0", no_split, 5.88),
              "");
    const auto m2k = closing_leg(2.0 * (2410.3 - 2400.0) * 5.0, 2431.0 - 2410.3, (2431.0 - 2410.3) * 2.0 * 5.0, 2.94);
    EXPECT_EQ(report_roll_line(ReportRollNames{"Micro E-Mini Russell-2000 Index", "September", "December", 1},
                               "M2K.v.0", m2k, 5.88),
              "Micro E-Mini Russell-2000 Index, September to December: market move booked $103; contract price "
              "difference 20.7 ($207, not profit or loss); roll cost $5.88.");
}

TEST(ReportRolls, TheRolloverWarningIsTheOldBoxWithTheRunDateAndTheDeadline) {
    ReportRolls rolls;
    rolls.per_contract_rolls = true;
    rolls.outlook["SI"] = outlook("SI", "SIZ5", true);
    rolls.outlook["GC"] = outlook("GC", "GCZ5", false);           // window not open
    auto micro = outlook("MES", "MESZ5", true);  // a micro's line is every root's
    micro.roll_by = "2025-12-18";
    micro.deadline = "2025-12-19";
    micro.first_notice = false;
    micro.target = "MESH6";
    rolls.outlook["MES"] = micro;
    auto jpy = outlook("6J", "JPYZ5", true);  // the IB symbol labels the line, as the removed box did
    jpy.label = "JPY";
    jpy.target = "JPYH6";
    rolls.outlook["6J"] = jpy;
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
    EXPECT_NE(box.find("<li><strong>MES:</strong> Currently holding <strong>MESZ5</strong> \xE2\x86\x92 Switch to "
                       "<strong>MESH6</strong> by 12-18, last trading day 12-19</li>"),
              std::string::npos)
        << box;
    EXPECT_EQ(box.find("moves to the next contract when trading does"), std::string::npos) << "the no-date line is gone";
    EXPECT_NE(box.find("<li><strong>JPY:</strong> Currently holding <strong>JPYZ5</strong> \xE2\x86\x92 Switch to "
                       "<strong>JPYH6</strong>"),
              std::string::npos)
        << box;
    rolls.outlook.erase("SI");
    rolls.outlook.erase("6L");
    rolls.outlook.erase("6J");
    rolls.outlook["MES"].in_window = false;
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
// outlook from its series; a root with no cycle, a series that fails and a read that throws get no outlook; each roll's months come from its two
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

    ReportRollSources src;
    src.series = [&](const std::string& root, Day) -> Result<ContractRollSeries> {
        if (root == "GC") return series;
        return make_error<ContractRollSeries>(ErrorCode::DATA_NOT_FOUND, "no contract bars", "test");
    };
    src.contracts_around = [&](const std::string& root, Day) -> Result<ContractBook> {
        if (root == "GC") return *book;
        throw std::runtime_error("unreadable");
    };
    src.names = [] { return std::map<std::string, ReportContractName>{{"GC", {"Gold ", "GC"}}}; };

    ReportRollQuery q;
    q.date = "2026-07-24";
    q.held_symbols = {"GC.v.0", "ZN.v.0", "XX.v.0"};
    q.rolls = {{"EXEC_GC.v.0_20260727_RC", "GC.v.0", "q", "z", "2026-07-27"},
               {"EXEC_ZN.v.0_20260727_RC", "ZN.v.0", "a", "b", "2026-07-27"}};
    const auto view = report_roll_view(q, src, [](const std::string& s) { return s == "GC.v.0" ? 0.1 : 0.0; });
    EXPECT_TRUE(view.per_contract_rolls);
    ASSERT_EQ(view.outlook.count("GC"), 1u);
    EXPECT_EQ(view.outlook.at("GC").held, "GCQ6");
    EXPECT_TRUE(view.outlook.at("GC").in_window);
    EXPECT_EQ(view.outlook.at("GC").roll_by, "2026-07-30");
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
    broken.names = []() -> std::map<std::string, ReportContractName> { throw std::runtime_error("no metadata"); };
    EXPECT_EQ(report_roll_view(q, broken, {}).names.at("EXEC_GC.v.0_20260727_RC").name, "");
    ReportRollQuery none = q;
    none.rolls.clear();
    EXPECT_TRUE(report_roll_view(none, src, {}).names.empty());
    ReportRollQuery early = q;
    early.date = "2026-07-21";  // before the GC series' first date: its outlook cannot be read, no outlook
    early.rolls.clear();
    EXPECT_EQ(report_roll_view(early, src, {}).outlook.count("GC"), 0u);
    ReportRollQuery bad = q;
    bad.date = "24 July";
    const auto empty = report_roll_view(bad, src, {});
    EXPECT_TRUE(empty.per_contract_rolls);
    EXPECT_TRUE(empty.outlook.empty());
    EXPECT_TRUE(empty.names.empty());
}

// The cells print the IB symbol, the month code and the year's last digit.
TEST(ReportRolls, TheContractsPrintInTheIbFormat) {
    EXPECT_EQ(report_ib_contract("JPY", "6J", "6JZ5"), "JPYZ5");
    EXPECT_EQ(report_ib_contract("NG", "NG", "NGV26"), "NGV6");
    EXPECT_EQ(report_ib_contract("", "SI", "SIZ5"), "SIZ5");
    EXPECT_EQ(report_ib_contract("GC", "GC", "SIZ5"), "SIZ5") << "not the root's code: as it is";
    EXPECT_EQ(report_ib_contract("MES", "ES", "ESZ5"), "MESZ5") << "a micro's full-size contract, the micro's month";
}

// The micros' outlook is the rule's, as every root's. Before the vendor's
// 2026-02-22 change MES's series holds the ES contract; the cell and the box print it as the micro of that
// month ("MESZ5"), with the run date and the deadline like every root. The book's
// id names a full-size contract too (the old one until its legs are booked). The window opens on the
// fifth CME trading day before the last trading day and closes after the last actionable bar's roll. Each
// roll's months come from its two contracts, the alias roll's (ESH6 to MESH6) March to March.
TEST(ReportRolls, TheMicrosOutlookIsTheRulesAndPrintsTheFullSizeContractAsTheMicros) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (Day x = d("2025-12-01"); x <= d("2026-03-06"); x += std::chrono::days{1}) {
        const auto wd = std::chrono::weekday{x};
        if (wd == std::chrono::Saturday) continue;
        const std::string dt = ymd(x);
        const bool sunday = wd == std::chrono::Sunday;
        if (dt <= "2025-12-19" && !sunday)
            rows.push_back(contract_rolls_test::bar("ES", "ESZ5", "e0", dt, "2025-12-19", 6800, 1500000));
        if (!sunday)
            rows.push_back(contract_rolls_test::bar("ES", "ESH6", "e1", dt, "2026-03-20", 6900,
                                                    dt <= "2025-12-12" ? 9000 : 1600000));
        // MES's own contracts print from the start too; before the change they are never read.
        if (dt <= "2025-12-19") rows.push_back(contract_rolls_test::bar("MES", "MESZ5", "mz", dt, "2025-12-19", 6801, 5));
        rows.push_back(contract_rolls_test::bar("MES", "MESH6", "m1", dt, "2026-03-20", 6901, 1700000));
        VendorBar v;
        v.date = x;
        v.symbol = "MES.v.0";
        v.instrument_id = dt >= "2026-02-22" ? "m1" : (dt <= "2025-12-12" ? "e0" : "e1");
        grid.push_back(v);
    }
    auto book = std::make_shared<ContractBook>(ContractBook::build(rows).value());
    const TradingCalendar cal = contract_rolls_test::shipped_calendar("MES");
    RuleInputs in;
    in.root = "MES";
    in.grid = grid;
    in.book = book.get();
    in.cal = &cal;
    in.cycle = "HMUZ";
    in.alias = micro_alias("MES");
    auto held = apply_roll_rule(in);
    ASSERT_TRUE(held.is_ok()) << held.error()->what();
    const ContractRollSeries series(held.value(), book, "HMUZ", micro_alias("MES"), cal);
    ReportRollSources src;
    src.series = [&](const std::string&, Day) -> Result<ContractRollSeries> { return series; };
    src.contracts_around = [&](const std::string&, Day) -> Result<ContractBook> { return *book; };
    src.names = [] {
        return std::map<std::string, ReportContractName>{{"MES", {"Micro E-mini S&P500 Index", "MES"}}};
    };
    ReportRollQuery q;
    q.held_symbols = {"MES.v.0"};
    q.held_ids = {{"MES.v.0", "e0"}};
    const auto view_on = [&](const std::string& date) {
        q.date = date;
        return report_roll_view(q, src, {});
    };
    const auto thu = view_on("2025-12-11").outlook.at("MES");
    EXPECT_EQ(thu.held_symbol, "ESZ5");
    EXPECT_EQ(thu.held, "MESZ5");
    EXPECT_FALSE(thu.in_window) << "the sixth CME trading day before 12-19";
    const auto fri_view = view_on("2025-12-12");
    const auto& fri = fri_view.outlook.at("MES");
    EXPECT_TRUE(fri.in_window) << "the fifth";
    EXPECT_EQ(fri.roll_by, "2025-12-18");
    EXPECT_EQ(fri.deadline, "2025-12-19");
    EXPECT_FALSE(fri.first_notice);
    EXPECT_EQ(fri.target, "MESH6");
    EXPECT_NE(report_rollover_warning_html(fri_view).find(
                  "<li><strong>MES:</strong> Currently holding <strong>MESZ5</strong> \xE2\x86\x92 Switch to "
                  "<strong>MESH6</strong> by 12-18, last trading day 12-19</li>"),
              std::string::npos)
        << report_rollover_warning_html(fri_view);
    // Mon 12-15: the rule has rolled into ESH6 and the book (id e0) still holds ESZ5: the old one, no line.
    const auto mon = view_on("2025-12-15").outlook.at("MES");
    EXPECT_EQ(mon.held, "MESZ5") << "the book's full-size contract, named by its id";
    EXPECT_FALSE(mon.in_window);
    q.held_ids = {{"MES.v.0", "e1"}};
    EXPECT_EQ(view_on("2026-02-20").outlook.at("MES").held, "MESH6") << "ESH6, the micro's March";
    q.held_ids = {{"MES.v.0", "m1"}};
    const auto after = view_on("2026-02-24").outlook.at("MES");
    EXPECT_EQ(after.held_symbol, "MESH6") << "the micro's own from the change";
    EXPECT_EQ(after.held, "MESH6");
    q.rolls = {{"EXEC_MES.v.0_20251215_RC", "MES.v.0", "e0", "e1", "2025-12-15"},
               {"EXEC_MES.v.0_20260223_RC", "MES.v.0", "e1", "m1", "2026-02-23"}};
    const auto rolled = view_on("2026-02-23");
    EXPECT_EQ(rolled.names.at("EXEC_MES.v.0_20251215_RC").from_month, "December");
    EXPECT_EQ(rolled.names.at("EXEC_MES.v.0_20251215_RC").to_month, "March");
    EXPECT_EQ(rolled.names.at("EXEC_MES.v.0_20260223_RC").from_month, "March");
    EXPECT_EQ(rolled.names.at("EXEC_MES.v.0_20260223_RC").to_month, "March");
    EXPECT_EQ(rolled.names.at("EXEC_MES.v.0_20260223_RC").name, "Micro E-mini S&P500 Index");
}

// A ruled root: the rule has rolled GC on the settled bar (a vendor switch the next bar confirms) and
// the book still holds the old contract: the cell names the old one and the box has no line for it yet.
TEST(ReportRolls, ARuledRootPendingItsLegsShowsTheOldContractAndNoLine) {
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
    ReportRollSources src;
    src.series = [&](const std::string&, Day) -> Result<ContractRollSeries> { return series; };
    src.contracts_around = [&](const std::string&, Day) -> Result<ContractBook> { return *book; };
    src.names = [] { return std::map<std::string, ReportContractName>{{"GC", {"Gold", "XGC"}}}; };
    ReportRollQuery q;
    q.date = "2026-07-24";
    q.held_symbols = {"GC.v.0"};
    q.held_ids = {{"GC.v.0", "q"}};  // the book's and the rule's contract agree
    const auto agree = report_roll_view(q, src, {}).outlook.at("GC");
    EXPECT_TRUE(agree.in_window);
    // S6: an IB symbol other than the Databento one labels the line and prints the contracts.
    EXPECT_EQ(agree.label, "XGC");
    EXPECT_EQ(agree.held, "XGCQ6");
    EXPECT_EQ(agree.held_symbol, "GCQ6");
    EXPECT_EQ(agree.target, "XGCZ6");
    q.held_ids = {{"GC.v.0", "z"}};  // stands in for a book that differs from the rule's contract
    const auto o = report_roll_view(q, src, {}).outlook.at("GC");
    EXPECT_EQ(o.held, "XGCZ6");
    EXPECT_FALSE(o.in_window) << "no line while the book and the rule differ";
}

// "First notice" is printed only when the notice day is the deadline; no roll footnote
// on a row with no quantity (nor on an equity row, nor on a symbol whose bar was no roll bar).
TEST(ReportRolls, FirstNoticeOnlyWhenTheNoticeDayIsTheDeadlineAndNoFootnoteOnAnEmptyRow) {
    ContractDates dates;
    dates.last_trading_day = d("2026-09-14");
    dates.deadline = d("2026-09-14");
    EXPECT_FALSE(report_deadline_is_first_notice(dates)) << "no notice day";
    dates.first_notice_day = d("2026-09-16");
    EXPECT_FALSE(report_deadline_is_first_notice(dates)) << "the notice day after the last trading day";
    dates.first_notice_day = d("2026-09-14");
    EXPECT_TRUE(report_deadline_is_first_notice(dates));
    SettledZeroKinds kinds;
    kinds.per_contract_roll = {"SI.v.0"};
    EXPECT_EQ(settled_roll_bar_note(false, "SI.v.0", 1.0, kinds), kSettledNoteRollBar);
    EXPECT_EQ(settled_roll_bar_note(false, "SI.v.0", -2.0, kinds), kSettledNoteRollBar);
    EXPECT_EQ(settled_roll_bar_note(false, "SI.v.0", 0.0, kinds), 0) << "no quantity: no footnote";
    EXPECT_EQ(settled_roll_bar_note(true, "SI.v.0", 1.0, kinds), 0) << "an equity row";
    EXPECT_EQ(settled_roll_bar_note(false, "GC.v.0", 1.0, kinds), 0) << "no roll bar";
}
