// tests/backtest/test_consumed_series_record.cpp
//
// The futures backtest's own record of what it consumed (consumed_series_record.hpp, T-ROLLX-FIX
// commit 1): the oracle is fed the engine's withheld set, hold set and rebalance calendar, and the
// engine's series is the one built on the bars it actually fed (LOOP_SPEC v6.1 section 11). A
// disabled record writes nothing; an enabled one writes the four files, the series built once over
// each symbol's whole consumed sequence.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/backtest/consumed_series_record.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace {

Timestamp wday(int d) { return Timestamp(std::chrono::seconds(1767571200LL + 86400LL * d)); }

Bar bar(const std::string& symbol, int d, double close, const std::string& id) {
    Bar b(wday(d), close, close, close, close, 1000.0, symbol);
    b.instrument_id = id;
    return b;
}

std::vector<std::string> lines(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::vector<std::string> out;
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}

SymbolDayVerdict verdict(const std::string& symbol, int d, SessionVerdict kind) {
    SymbolDayVerdict v;
    v.symbol = symbol;
    v.date = SessionClassifier::ymd(SessionClassifier::day_of(wday(d)));
    v.verdict = kind;
    v.has_bar = true;
    return v;
}

}  // namespace

TEST(ConsumedSeriesRecord, ADisabledRecordWritesNothing) {
    const auto dir = std::filesystem::temp_directory_path() / "tngin_csr_disabled";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ConsumedSeriesRecord r;
    r.add_cycle("2026-01-05", {verdict("XA", 0, SessionVerdict::JUNK)}, {bar("XA", 0, 10, "A")}, {});
    EXPECT_TRUE(r.write());
    EXPECT_TRUE(std::filesystem::is_empty(dir));
    std::filesystem::remove_all(dir);
}

TEST(ConsumedSeriesRecord, TheFourFilesCarryTheCyclesAndTheSeriesOfTheFedBarsOnly) {
    const auto dir = std::filesystem::temp_directory_path() / "tngin_csr_enabled";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ConsumedSeriesRecord r;
    r.enable(dir.string());
    // XA: A A [withheld B print] B B: the withheld bar is in withheld.csv and in no series row,
    // so the change bar is day 3 (the first FED bar on B) and day 4 confirms it.
    r.add_cycle("2026-01-05", {verdict("XA", 0, SessionVerdict::SESSION)}, {}, {bar("XA", 0, 100, "A")});
    r.add_cycle("2026-01-06", {verdict("XA", 1, SessionVerdict::SESSION)}, {}, {bar("XA", 1, 101, "A")});
    r.add_cycle("2026-01-07", {verdict("XA", 2, SessionVerdict::JUNK)}, {bar("XA", 2, 50, "B")}, {});
    r.add_cycle("2026-01-08", {verdict("XA", 3, SessionVerdict::SESSION)}, {}, {bar("XA", 3, 110, "B")});
    r.add_cycle("2026-01-09", {verdict("XA", 4, SessionVerdict::SESSION)}, {}, {bar("XA", 4, 111, "B")});
    ASSERT_TRUE(r.write());
    EXPECT_EQ(lines(dir / "calendar.csv"),
              (std::vector<std::string>{"date", "2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08",
                                        "2026-01-09"}));
    EXPECT_EQ(lines(dir / "withheld.csv"), (std::vector<std::string>{"date,symbol", "2026-01-07,XA"}));
    // L-07 (F-5): a withheld date is a hold without a hold-set row.
    EXPECT_EQ(lines(dir / "hold.csv"), (std::vector<std::string>{"date,symbol"}));
    const auto s = lines(dir / "series.csv");
    ASSERT_EQ(s.size(), 5u) << "a header and the four FED bars";
    EXPECT_EQ(s[0], "symbol,date,close,instrument_id,change,confirm,flip,pending,held_id,A,r");
    const auto field = [](const std::string& row, int k) {
        std::stringstream ss(row);
        std::string f;
        for (int i = 0; i <= k; ++i) std::getline(ss, f, ',');
        return f;
    };
    const auto flags = [&](const std::string& row) {
        std::string out;
        for (int k = 0; k <= 8; ++k) out += field(row, k) + (k < 8 ? "," : "");
        return out;
    };
    EXPECT_EQ(flags(s[1]), "XA,2026-01-05,100,A,0,0,0,0,A");
    EXPECT_EQ(flags(s[2]), "XA,2026-01-06,101,A,0,0,0,0,A");
    EXPECT_EQ(flags(s[3]), "XA,2026-01-08,110,B,1,0,0,1,A")
        << "the first FED bar on B is the change bar: pending, held in A";
    EXPECT_EQ(flags(s[4]), "XA,2026-01-09,111,B,0,1,0,0,B")
        << "the next fed bar keeps B: the roll is confirmed";
    // A = raw + the later steps (110 - 101 on the change bar); r = the adjusted change over the raw
    // previous close, 0 on the change bar.
    EXPECT_DOUBLE_EQ(std::stod(field(s[1], 9)), 109.0);
    EXPECT_DOUBLE_EQ(std::stod(field(s[2], 9)), 110.0);
    EXPECT_DOUBLE_EQ(std::stod(field(s[3], 9)), 110.0) << "A is flat across the step";
    EXPECT_DOUBLE_EQ(std::stod(field(s[4], 9)), 111.0);
    EXPECT_DOUBLE_EQ(std::stod(field(s[1], 10)), 0.0) << "the first consumed bar";
    EXPECT_DOUBLE_EQ(std::stod(field(s[2], 10)), 0.01);
    EXPECT_DOUBLE_EQ(std::stod(field(s[3], 10)), 0.0) << "the change bar's return";
    EXPECT_DOUBLE_EQ(std::stod(field(s[4], 10)), 1.0 / 110.0);
    std::filesystem::remove_all(dir);
}

// LOOP_SPEC section 6.1: a symbol with no verdict in the signal group is held, and the hold set the
// reference is fed must say so. hold.csv listed only the symbols with a bad bar, so a checker fed
// the file alone traded a symbol on a day it printed no bar.
TEST(ConsumedSeriesRecord, ASymbolThatPrintsNoBarInTheGroupIsInTheHoldSet) {
    const auto dir = std::filesystem::temp_directory_path() / "tngin_csr_no_bar_hold";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ConsumedSeriesRecord r;
    r.enable(dir.string());
    // XH is known from the history before the window; XA and XB print on the first cycle.
    r.add_history({}, {bar("XH", -1, 50, "H")});
    r.add_cycle("2026-01-05",
                {verdict("XA", 0, SessionVerdict::SESSION), verdict("XB", 0, SessionVerdict::SESSION)}, {},
                {bar("XA", 0, 100, "A"), bar("XB", 0, 200, "B")});
    // The second cycle: XA prints a session, XB prints a bar that is withheld, XH prints nothing
    // again, and XN prints for the first time.
    r.add_cycle("2026-01-06",
                {verdict("XA", 1, SessionVerdict::SESSION), verdict("XB", 1, SessionVerdict::JUNK),
                 verdict("XN", 1, SessionVerdict::SESSION)},
                {bar("XB", 1, 1, "B")}, {bar("XA", 1, 101, "A"), bar("XN", 1, 10, "N")});
    // The third cycle: only XA prints. XB (last seen withheld) and XN are held with XH.
    r.add_cycle("2026-01-07", {verdict("XA", 2, SessionVerdict::SESSION)}, {}, {bar("XA", 2, 102, "A")});
    ASSERT_TRUE(r.write());
    EXPECT_EQ(lines(dir / "hold.csv"),
              (std::vector<std::string>{"date,symbol", "2026-01-05,XH", "2026-01-06,XH", "2026-01-07,XB",
                                        "2026-01-07,XH", "2026-01-07,XN"}))
        << "every known symbol with no bar in the group; a withheld bar's date has no row (L-07); "
           "a symbol is not held before its first bar";
    std::filesystem::remove_all(dir);
}

// A run starts from empty files: enabling the record removes every record file an earlier run
// left in the directory, so a run that stops before its last write cannot leave its one-pass rows
// beside the earlier run's series. Files that are not the record's are left alone.
TEST(ConsumedSeriesRecord, EnablingTheRecordRemovesAnEarlierRunsFiles) {
    const auto dir = std::filesystem::temp_directory_path() / "tngin_csr_second_run";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::vector<std::string> record = {"calendar.csv", "hold.csv", "withheld.csv", "series.csv",
                                             "history.csv", "final_marks.csv", "onepass_days_PM.csv",
                                             "onepass_book_PM.csv", "estimator_TREND_FOLLOWING.csv"};
    const std::vector<std::string> other = {"notes.txt", "series.csv.gz", "estimator.txt"};
    for (const auto& name : record) std::ofstream(dir / name) << "an earlier run\n";
    for (const auto& name : other) std::ofstream(dir / name) << "not the record's\n";
    ConsumedSeriesRecord r;
    r.enable(dir.string());
    for (const auto& name : record) EXPECT_FALSE(std::filesystem::exists(dir / name)) << name;
    for (const auto& name : other) EXPECT_TRUE(std::filesystem::exists(dir / name)) << name;
    std::filesystem::remove_all(dir);
}

// T-ROLLX-FIX commit 2: the last marked group (never a signal group) is written with the marks' own
// reading: withheld, change, the contract held after the bar.
TEST(ConsumedSeriesRecord, TheFinalMarkedGroupIsWrittenWithTheMarksReading) {
    const auto dir = std::filesystem::temp_directory_path() / "tngin_csr_final";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ConsumedSeriesRecord r;
    r.enable(dir.string());
    ConsumedSeriesRecord::FinalMark a;
    a.symbol = "XA";
    a.date = "2026-01-09";
    a.close = 111.0;
    a.instrument_id = "B";
    a.change = true;
    a.held_id = "A";
    ConsumedSeriesRecord::FinalMark b = a;
    b.symbol = "XB";
    b.change = false;
    b.withheld = true;
    b.held_id = "Z";
    r.set_final_marks({a});
    r.set_final_marks({a, b});  // the last call is kept
    ASSERT_TRUE(r.write());
    EXPECT_EQ(lines(dir / "final_marks.csv"),
              (std::vector<std::string>{"symbol,date,close,instrument_id,withheld,change,held_id",
                                        "XA,2026-01-09,111,B,0,1,A", "XB,2026-01-09,111,B,1,0,Z"}));
    std::filesystem::remove_all(dir);
}
