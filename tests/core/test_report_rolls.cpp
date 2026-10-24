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
    // Short 2 contracts of a Treasury, the new contract cheaper by 0.5: the difference prints without a
    // sign (lead, R4b fix round 1: it is not profit or loss); the stored values keep theirs.
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
    // A roll of the vendor's series (no split): the cost only, with the reason.
    ExecutionReport no_split = leg;
    no_split.roll_bar_move.reset();
    no_split.roll_gap_points.reset();
    no_split.roll_gap_value.reset();
    EXPECT_EQ(report_roll_line(ReportRollNames{"Micro E-Mini Russell-2000 Index", "September", "December", 1},
                               "M2K.v.0", no_split, 5.88),
              "Micro E-Mini Russell-2000 Index, September to December: roll cost $5.88 (the market move and the "
              "price difference between the two contracts are not shown for this contract yet).");
}

TEST(ReportRolls, TheRolloverWarningIsTheOldBoxWithTheRunDateAndTheDeadline) {
    ReportRolls rolls;
    rolls.per_contract_rolls = true;
    rolls.outlook["SI"] = outlook("SI", "SIZ5", true);
    rolls.outlook["GC"] = outlook("GC", "GCZ5", false);           // window not open
    auto micro = outlook("MES", "MESZ5", true, false);  // the vendor's series: its last trading day only
    micro.deadline = "2025-12-19";
    micro.roll_by.clear();
    micro.target.clear();
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
    EXPECT_NE(box.find("<li><strong>MES:</strong> Currently holding <strong>MESZ5</strong>, moves to the next "
                       "contract when trading does, before the contract ends on 12-19</li>"),
              std::string::npos)
        << box;
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
    src.names = [] {
        return std::map<std::string, ReportContractName>{{"GC", {"Gold ", "GC"}}, {"MES", {"Micro S&P", "MES"}}};
    };
    src.calendar = [](const std::string& root) { return contract_rolls_test::shipped_calendars().for_root(root); };

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
    broken.names = []() -> std::map<std::string, ReportContractName> { throw std::runtime_error("no metadata"); };
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

// M-1 (R4B audit B): the Front Month and the box name the contract the BOOK holds (the roll tracker's id),
// so the old contract until its legs are booked, for every root. A micro whose vendor already names the
// December contract while the book still holds September shows September, with its last trading day while
// its window is open; a ruled root whose rule has rolled on the settled bar while the book holds the old
// contract shows the old one and no line. S6: the cells print the IB symbol with the month code.
TEST(ReportRolls, TheViewNamesTheContractTheBookHolds) {
    const auto micro = ContractBook::build({contract_rolls_test::bar("M2K", "M2KU6", "u", "2026-09-16", "2026-09-18", 2877, 100),
                                            contract_rolls_test::bar("M2K", "M2KZ6", "z", "2026-09-16", "2026-12-18", 2900, 900),
                                            contract_rolls_test::bar("6J", "6JZ6", "jz", "2026-09-16", "2026-12-14", 0.0068, 900),
                                            contract_rolls_test::bar("6J", "6JU6", "ju", "2026-09-16", "2026-09-14", 0.0067, 10)})
                           .value();
    ReportRollSources src;
    src.vendor_grid = [](const std::string& root, Day, Day) {
        VendorBar v;
        v.date = d("2026-09-16");
        v.symbol = root + ".v.0";
        v.instrument_id = "z";  // the vendor already names December
        return std::vector<VendorBar>{v};
    };
    src.contracts_around = [&](const std::string&, Day) -> Result<ContractBook> { return micro; };
    src.calendar = [](const std::string& root) { return contract_rolls_test::shipped_calendars().for_root(root); };
    src.names = [] {
        return std::map<std::string, ReportContractName>{{"M2K", {"Micro E-Mini Russell-2000 Index", "M2K"}},
                                                         {"6J", {"JPY/USD", "JPY"}}};
    };
    src.series = [](const std::string&, Day) -> Result<ContractRollSeries> {
        return make_error<ContractRollSeries>(ErrorCode::DATA_NOT_FOUND, "not used", "test");
    };
    ReportRollQuery q;
    q.date = "2026-09-16";
    q.held_symbols = {"M2K.v.0"};
    q.held_ids = {{"M2K.v.0", "u"}};
    const auto view = report_roll_view(q, src, {});
    const auto& o = view.outlook.at("M2K");
    EXPECT_EQ(o.held, "M2KU6") << "the book's September, not the vendor's December";
    EXPECT_EQ(o.held_symbol, "M2KU6");
    EXPECT_EQ(o.label, "M2K");
    EXPECT_FALSE(o.rule_rolls);
    EXPECT_EQ(o.deadline, "2026-09-18");
    EXPECT_TRUE(o.in_window) << "five CME trading days before 09-18 is 09-11";
    EXPECT_NE(report_rollover_warning_html(view).find("M2K:</strong> Currently holding <strong>M2KU6</strong>, moves "
                                                      "to the next contract when trading does, before the "
                                                      "contract ends on 09-18"),
              std::string::npos);
    // Without the book's id the vendor's contract is the fallback.
    q.held_ids.clear();
    EXPECT_EQ(report_roll_view(q, src, {}).outlook.at("M2K").held, "M2KZ6");
    // After the last trading day: no line.
    q.held_ids = {{"M2K.v.0", "u"}};
    q.date = "2026-09-19";
    EXPECT_FALSE(report_roll_view(q, src, {}).outlook.at("M2K").in_window);
    // S6: the IB format, the IB symbol, the month code, the year's last digit.
    EXPECT_EQ(report_ib_contract("JPY", "6J", "6JZ5"), "JPYZ5");
    EXPECT_EQ(report_ib_contract("NG", "NG", "NGV26"), "NGV6");
    EXPECT_EQ(report_ib_contract("", "SI", "SIZ5"), "SIZ5");
    EXPECT_EQ(report_ib_contract("GC", "GC", "SIZ5"), "SIZ5") << "not the root's code: as it is";
}

// R4b audit A recheck R2: the micros' warning window, at its bounds. M2KU6's last trading day is Fri
// 2026-09-18; the window opens on the fifth CME trading day before it (Fri 09-11) and closes on it. The
// sixth trading day before (Thu 09-10) and the day after (Sat 09-19, the vendor's last bar still naming
// September) show no line; 09-11 and 09-18 show it.
TEST(ReportRolls, TheMicrosWindowOpensOnTheFifthTradingDayBeforeTheLastTradingDayAndClosesOnIt) {
    const auto book = ContractBook::build({contract_rolls_test::bar("M2K", "M2KU6", "u", "2026-09-10", "2026-09-18", 2870, 900),
                                           contract_rolls_test::bar("M2K", "M2KU6", "u", "2026-09-11", "2026-09-18", 2871, 900),
                                           contract_rolls_test::bar("M2K", "M2KU6", "u", "2026-09-18", "2026-09-18", 2877, 100),
                                           contract_rolls_test::bar("M2K", "M2KZ6", "z", "2026-09-18", "2026-12-18", 2900, 900)})
                          .value();
    ReportRollSources src;
    src.vendor_grid = [](const std::string& root, Day, Day to) {
        VendorBar v;
        v.date = std::min(to, d("2026-09-18"));
        v.symbol = root + ".v.0";
        v.instrument_id = "u";
        return std::vector<VendorBar>{v};
    };
    src.contracts_around = [&](const std::string&, Day) -> Result<ContractBook> { return book; };
    src.calendar = [](const std::string& root) { return contract_rolls_test::shipped_calendars().for_root(root); };
    src.names = [] { return std::map<std::string, ReportContractName>{{"M2K", {"Micro Russell 2000", "M2K"}}}; };
    src.series = [](const std::string&, Day) -> Result<ContractRollSeries> {
        return make_error<ContractRollSeries>(ErrorCode::DATA_NOT_FOUND, "not used", "test");
    };
    ReportRollQuery q;
    q.held_symbols = {"M2K.v.0"};
    q.held_ids = {{"M2K.v.0", "u"}};
    const auto view_on = [&](const std::string& date) {
        q.date = date;
        return report_roll_view(q, src, {});
    };
    for (const auto& [date, open] : std::vector<std::pair<std::string, bool>>{
             {"2026-09-10", false}, {"2026-09-11", true}, {"2026-09-18", true}, {"2026-09-19", false}}) {
        const auto view = view_on(date);
        const auto& o = view.outlook.at("M2K");
        EXPECT_EQ(o.held, "M2KU6") << date;
        EXPECT_EQ(o.deadline, "2026-09-18") << date;
        EXPECT_EQ(o.in_window, open) << date;
        EXPECT_EQ(report_rollover_warning_html(view).find("M2K:</strong> Currently holding") != std::string::npos, open)
            << date;
    }
}

// M-1, a ruled root: the rule has rolled GC on the settled bar (a vendor switch the next bar confirms) and
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
    src.vendor_grid = [](const std::string&, Day, Day) { return std::vector<VendorBar>{}; };
    src.calendar = [](const std::string& root) { return contract_rolls_test::shipped_calendars().for_root(root); };
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

// S7 (R4B audit A): m06, "first notice" only when the notice day is the deadline; m10, no roll footnote
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
