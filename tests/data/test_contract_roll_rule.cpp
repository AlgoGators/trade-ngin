// The per-contract roll rule (contract_roll_rule.hpp) on hand-built GC-like fixtures, on
// the shipped trading calendar. GC's cycle is G J M Q Z; its deadline is the first notice day (the last
// trading day of the month before delivery). For GCQ6: D* = Fri 2026-07-31, the window opens Fri
// 07-24, the last actionable bar (LAB) is Wed 07-29 (its run is Thu 07-30).
#include <gtest/gtest.h>
#include <set>
#include <functional>

#include <map>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/contract_roll_rule.hpp"

using namespace trade_ngin::contract_rolls;
using trade_ngin::Result;
using contract_rolls_test::bar;
using contract_rolls_test::d;

namespace {

struct Spec {
    const char* raw;
    const char* id;
    const char* expiration;  // "" = NULL
};

/// A root's contracts, their bars and the vendor's grid, built day by day.
class World {
public:
    explicit World(std::vector<Spec> specs, std::string root = "GC") : root_(std::move(root)) {
        for (const auto& s : specs) specs_[s.raw] = s;
    }
    /// A contract's bar on a date.
    World& print(const std::string& raw, const std::string& date, int64_t volume, double close = 0) {
        const Spec& s = specs_.at(raw);
        rows_.push_back(bar(root_, raw, s.id, date, s.expiration, close ? close : 1000.0 + volume % 97,
                            volume));
        return *this;
    }
    /// The vendor's grid bar on a date, naming a contract.
    World& vendor(const std::string& date, const std::string& raw, double close = 1.0) {
        VendorBar v;
        v.date = d(date);
        v.symbol = root_ + ".v.0";
        v.instrument_id = specs_.at(raw).id;
        v.open = v.high = v.low = v.close = close;
        v.volume = 1;
        grid_.push_back(v);
        return *this;
    }
    Result<HeldSeries> run() {
        book_ = std::make_unique<ContractBook>(ContractBook::build(rows_).value());
        cal_ = std::make_unique<TradingCalendar>(contract_rolls_test::shipped_calendar(root_));
        RuleInputs in;
        in.root = root_;
        in.grid = grid_;
        in.book = book_.get();
        in.cal = cal_.get();
        in.cycle = roll_cycle(root_).value();
        in.log_from = log_from_;
        in.log_once = log_once_;
        return apply_roll_rule(in);
    }
    /// Asked before a warning is logged (RuleInputs::log_once).
    World& log_once(std::function<bool(const std::string&)> f) {
        log_once_ = std::move(f);
        return *this;
    }
    /// Warnings dated before `date` are kept, not logged (RuleInputs::log_from).
    World& log_from(const std::string& date) {
        log_from_ = d(date);
        return *this;
    }

private:
    std::string root_;
    std::map<std::string, Spec> specs_;
    std::vector<ContractBar> rows_;
    std::vector<VendorBar> grid_;
    std::unique_ptr<ContractBook> book_;
    std::unique_ptr<TradingCalendar> cal_;
    Day log_from_{};
    std::function<bool(const std::string&)> log_once_;
};

const std::vector<Spec> kAug2026 = {{"GCQ6", "q", "2026-08-27"},
                                    {"GCU6", "u", "2026-09-28"},
                                    {"GCV6", "v", "2026-10-28"},
                                    {"GCZ6", "z", "2026-12-29"},
                                    {"GCG7", "g", "2027-02-24"}};

// GC's grid has Sunday bars and no Saturday bars. 2026-07-15 (Wed) to 08-05 (Wed).
const std::vector<std::string> kDates = {
    "2026-07-15", "2026-07-16", "2026-07-17", "2026-07-19", "2026-07-20", "2026-07-21",
    "2026-07-22", "2026-07-23", "2026-07-24", "2026-07-26", "2026-07-27", "2026-07-28",
    "2026-07-29", "2026-07-30", "2026-07-31", "2026-08-02", "2026-08-03", "2026-08-04",
    "2026-08-05"};

/// Every contract prints every grid date; Q is the volume leader, Z second, the off-cycle U and V
/// are busier than Z (so a rule that ignored the cycle would pick them).
World base_world() {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        w.print("GCQ6", dt, 80000).print("GCU6", dt, 30000).print("GCV6", dt, 20000);
        w.print("GCZ6", dt, 9000).print("GCG7", dt, 500);
    }
    return w;
}

const HeldDay& on(const HeldSeries& s, const std::string& date) {
    for (const auto& day : s.days) {
        if (day.date == d(date)) return day;
    }
    throw std::out_of_range("no grid date " + date);
}

/// The rule's warnings that contain `text` (each fallback's WARN in the run's log).
size_t warned(const HeldSeries& s, const std::string& text) {
    size_t n = 0;
    for (const auto& w : s.report.warnings)
        n += w.find(text) != std::string::npos;
    return n;
}

std::vector<std::string> roll_dates(const HeldSeries& s) {
    std::vector<std::string> out;
    for (const auto& day : s.days) {
        if (day.roll) out.push_back(ymd(day.date));
    }
    return out;
}

}  // namespace

TEST(ContractRollRule, ContractDatesFridayAndMondayFirstNotice) {
    const TradingCalendar cal = contract_rolls_test::shipped_calendar();
    // Friday first notice (GCQ6): LAB is Wednesday's bar, traded by Thursday's run.
    const ContractDates q6 =
        contract_dates(ContractKey::make("GC", "GCQ6", d("2026-08-27")).value(), cal);
    EXPECT_EQ(ymd(*q6.first_notice_day), "2026-07-31");
    EXPECT_EQ(ymd(q6.deadline), "2026-07-31");
    EXPECT_EQ(ymd(q6.window_first), "2026-07-24");
    EXPECT_EQ(ymd(q6.last_actionable), "2026-07-29");
    EXPECT_EQ(ymd(*q6.rule_last_trading_day), "2026-08-27");
    // Monday first notice (GCJ5, 2025-03-31): LAB is Friday's bar, traded by Saturday's run.
    const ContractDates j5 =
        contract_dates(ContractKey::make("GC", "GCJ5", d("2025-04-28")).value(), cal);
    EXPECT_EQ(ymd(j5.deadline), "2025-03-31");
    EXPECT_EQ(ymd(j5.last_actionable), "2025-03-28");
    EXPECT_EQ(ymd(j5.window_first), "2025-03-24");
    // No notice day before the last trade: D* is the last trading day (MBTQ6, Friday 08-28).
    const ContractDates mbt = contract_dates(ContractKey::make("MBT", "MBTQ6", d("2026-08-28")).value(),
                                             contract_rolls_test::shipped_calendar("MBT"));
    EXPECT_FALSE(mbt.first_notice_day.has_value());
    EXPECT_EQ(ymd(mbt.deadline), "2026-08-28");
    EXPECT_EQ(ymd(mbt.last_actionable), "2026-08-26");
    // A holiday inside the window (Thanksgiving 2025-11-27): GCZ5's window is five trading days.
    const ContractDates z5 =
        contract_dates(ContractKey::make("GC", "GCZ5", d("2025-12-29")).value(), cal);
    EXPECT_EQ(ymd(z5.deadline), "2025-11-28");
    EXPECT_EQ(ymd(z5.window_first), "2025-11-20");
    EXPECT_EQ(ymd(z5.last_actionable), "2025-11-26");
}

TEST(ContractRollRule, DeadlineRollsOnTheLastActionableBarIntoTheCycleFront) {
    World w = base_world();
    for (const auto& dt : kDates) w.vendor(dt, "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldSeries& s = r.value();
    EXPECT_EQ(roll_dates(s), std::vector<std::string>{"2026-07-29"});
    const HeldDay& roll = on(s, "2026-07-29");
    EXPECT_EQ(roll.trigger, RollTrigger::DEADLINE);
    EXPECT_EQ(roll.old->raw_symbol, "GCQ6");
    EXPECT_EQ(roll.held.raw_symbol, "GCZ6");  // U and V are busier but not in the cycle
    EXPECT_FALSE(roll.target_skipped_front);
    EXPECT_FALSE(roll.late);
    ASSERT_TRUE(roll.old_close.has_value());
    EXPECT_EQ(*roll.old_close, 1000.0 + 80000 % 97);
    EXPECT_EQ(on(s, "2026-07-28").held.raw_symbol, "GCQ6");
    EXPECT_EQ(s.days.front().trigger, RollTrigger::START);
    EXPECT_EQ(s.report.rolls_by_trigger.at("deadline"), 1);
    // The vendor still names GCQ6 after the roll: an earlier contract, ignored; Z's own bar is read.
    EXPECT_EQ(on(s, "2026-07-30").held.raw_symbol, "GCZ6");
    EXPECT_EQ(on(s, "2026-07-30").bar_id, "z");
    EXPECT_EQ(on(s, "2026-07-30").source, BarSource::OWN);
}

TEST(ContractRollRule, CrossoverInsideTheWindowRollsFirst) {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        // Z overtakes Q on Monday 07-27 (inside the window) and on Friday 07-17 (outside it).
        const bool z_leads = dt == "2026-07-27" || dt == "2026-07-17" || dt >= "2026-07-28";
        w.print("GCQ6", dt, z_leads ? 5000 : 80000).print("GCZ6", dt, z_leads ? 90000 : 9000);
        w.vendor(dt, "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-27"});
    EXPECT_EQ(on(r.value(), "2026-07-27").trigger, RollTrigger::CROSSOVER);
}

TEST(ContractRollRule, TheVendorsConfirmedForwardSwitchRollsFirst) {
    World w = base_world();
    for (const auto& dt : kDates) w.vendor(dt, dt >= "2026-07-21" ? "GCZ6" : "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-21"});
    const HeldDay& roll = on(r.value(), "2026-07-21");
    EXPECT_EQ(roll.trigger, RollTrigger::VENDOR);
    EXPECT_FALSE(roll.deferred);
    EXPECT_FALSE(roll.vendor_off_cycle);
}

TEST(ContractRollRule, AVendorSwitchOnASundayRollsOnMonday) {
    World w = base_world();
    for (const auto& dt : kDates) w.vendor(dt, dt >= "2026-07-19" ? "GCZ6" : "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-20"});
    const HeldDay& roll = on(r.value(), "2026-07-20");
    EXPECT_EQ(roll.trigger, RollTrigger::VENDOR);
    EXPECT_EQ(roll.trigger_date, d("2026-07-19"));
    EXPECT_TRUE(roll.deferred);
    EXPECT_EQ(on(r.value(), "2026-07-19").held.raw_symbol, "GCQ6");
}

TEST(ContractRollRule, AVendorSwitchOutsideTheCycleTimesTheRollIntoTheCycleFront) {
    World w = base_world();
    for (const auto& dt : kDates) w.vendor(dt, dt >= "2026-07-21" ? "GCU6" : "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-21"});
    const HeldDay& roll = on(r.value(), "2026-07-21");
    EXPECT_EQ(roll.held.raw_symbol, "GCZ6");
    EXPECT_TRUE(roll.vendor_off_cycle);
    EXPECT_EQ(r.value().report.vendor_off_cycle, 1);
    // After it the vendor's U is earlier than the held Z: ignored.
    EXPECT_EQ(on(r.value(), "2026-07-24").held.raw_symbol, "GCZ6");
}

TEST(ContractRollRule, NeverBackAndOneBarStraysAreIgnored) {
    World w = base_world();
    for (const auto& dt : kDates) {
        std::string v = dt >= "2026-07-20" ? "GCZ6" : "GCQ6";
        if (dt == "2026-07-16") v = "GCZ6";                        // one-bar stray forward
        if (dt == "2026-07-22" || dt == "2026-07-23") v = "GCQ6";  // two bars back
        w.vendor(dt, v, 7.0);
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldSeries& s = r.value();
    EXPECT_EQ(roll_dates(s), std::vector<std::string>{"2026-07-20"});
    EXPECT_EQ(on(s, "2026-07-16").held.raw_symbol, "GCQ6");
    EXPECT_EQ(on(s, "2026-07-16").bar_id, "q");
    for (const char* back : {"2026-07-22", "2026-07-23"}) {
        EXPECT_EQ(on(s, back).held.raw_symbol, "GCZ6") << back;
        EXPECT_EQ(on(s, back).bar_id, "z") << back;
        EXPECT_EQ(on(s, back).close, 1000.0 + 9000 % 97) << back;
        EXPECT_EQ(on(s, back).vendor->raw_symbol, "GCQ6") << back;
    }
    // The vendor's return to Z after the backward visit is not a switch forward of the held Z.
    EXPECT_EQ(s.report.rolls, 1);
}

TEST(ContractRollRule, MondayFirstNoticeRollsOnFridaysBar) {
    // GCJ5: first notice Monday 2025-03-31; LAB = Friday 03-28 (traded by the Saturday run).
    World w({{"GCJ5", "j", "2025-04-28"}, {"GCK5", "k", "2025-05-28"}, {"GCM5", "m", "2025-06-26"}});
    for (const char* dt : {"2025-03-24", "2025-03-25", "2025-03-26", "2025-03-27", "2025-03-28",
                           "2025-03-30", "2025-03-31", "2025-04-01"}) {
        w.print("GCJ5", dt, 80000).print("GCK5", dt, 30000).print("GCM5", dt, 9000).vendor(dt, "GCJ5");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2025-03-28"});
    EXPECT_EQ(on(r.value(), "2025-03-28").trigger, RollTrigger::DEADLINE);
    EXPECT_EQ(on(r.value(), "2025-03-28").held.raw_symbol, "GCM5");  // May is not in the cycle
}

TEST(ContractRollRule, AHolidayInsideTheWindowCountsOnlyTradingDays) {
    // GCZ5: D* = Fri 2025-11-28; with Thanksgiving on 11-27 the window's five trading days start on
    // Thu 11-20. A crossover on 11-20 is inside it (it would not be if the holiday counted).
    World w({{"GCZ5", "z", "2025-12-29"}, {"GCG6", "g", "2026-02-25"}});
    for (const char* dt : {"2025-11-18", "2025-11-19", "2025-11-20", "2025-11-21", "2025-11-23",
                           "2025-11-24", "2025-11-25", "2025-11-26", "2025-11-27"}) {
        const bool g_leads = std::string(dt) == "2025-11-20" || std::string(dt) == "2025-11-19";
        w.print("GCZ5", dt, g_leads ? 1000 : 90000).print("GCG6", dt, g_leads ? 50000 : 2000);
        w.vendor(dt, "GCZ5");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    // 11-19 is before the window: no roll; 11-20 is its first day.
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2025-11-20"});
    EXPECT_EQ(on(r.value(), "2025-11-20").trigger, RollTrigger::CROSSOVER);
}

TEST(ContractRollRule, ClassCCarriesOrReadsTheVendorsBar) {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        if (dt != "2026-07-16" && dt != "2026-07-17") w.print("GCQ6", dt, 80000, 3300.0 + dt[9]);
        w.print("GCZ6", dt, 9000);
    }
    for (const auto& dt : kDates) {
        // 07-16: the vendor names Q, which has no row (its own print: the vendor's bar).
        // 07-17: the vendor names Z for one bar, Q has no row: carry Q's last close (07-15).
        w.vendor(dt, dt == "2026-07-17" ? "GCZ6" : "GCQ6", 4444.0);
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& v = on(r.value(), "2026-07-16");
    EXPECT_EQ(v.source, BarSource::VENDOR);
    EXPECT_EQ(v.close, 4444.0);
    EXPECT_EQ(v.bar_id, "q");
    const HeldDay& c = on(r.value(), "2026-07-17");
    EXPECT_EQ(c.source, BarSource::CARRY);
    EXPECT_EQ(c.close, 3300.0 + '5');
    EXPECT_EQ(c.held.raw_symbol, "GCQ6");
    EXPECT_EQ(c.bar_id, "");
    EXPECT_EQ(r.value().report.carry_days, 1);
    EXPECT_EQ(r.value().report.vendor_bar_days, 1);
    // Each in the log, naming the symbol, the date and the fallback.
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-16 fallback vendor_bar: GCQ6 has no contract row and "
                     "the vendor names it: the bar is the vendor's own print (close "
                     "4444)"),
              1u);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-17 fallback carry: GCQ6 has no contract row and the "
                     "vendor names GCZ6: its last close 3353 (2026-07-15) is carried"),
              1u);
    EXPECT_EQ(r.value().report.warnings.size(), 2u);
    EXPECT_EQ(r.value().report.warnings_not_logged, 0);
}

TEST(ContractRollRule, OldContractWithNoBarMovesToTheNextBarWhereBothPrint) {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        if (dt != "2026-07-21") w.print("GCQ6", dt, 80000);
        w.print("GCZ6", dt, 9000);
        w.vendor(dt, dt >= "2026-07-21" ? "GCZ6" : "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-22"});
    const HeldDay& roll = on(r.value(), "2026-07-22");
    EXPECT_TRUE(roll.deferred);
    EXPECT_EQ(roll.trigger_date, d("2026-07-21"));
    EXPECT_EQ(roll.fallback, RollFallback::NONE);
    // The waiting day: Q held, carried (the vendor names Z).
    EXPECT_EQ(on(r.value(), "2026-07-21").held.raw_symbol, "GCQ6");
    EXPECT_EQ(on(r.value(), "2026-07-21").source, BarSource::CARRY);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-21 fallback deferred: a roll from GCQ6 to GCZ6 is "
                     "due and GCQ6 has no bar: it waits for a bar where both print"),
              1u);
    EXPECT_EQ(warned(r.value(), "GC.v.0 2026-07-21 fallback carry: GCQ6"), 1u);
    EXPECT_EQ(r.value().report.warnings.size(), 2u);
}

TEST(ContractRollRule, OldContractWithNoBarUpToTheLastActionableBarFallsBack) {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        if (dt < "2026-07-29") w.print("GCQ6", dt, 80000);  // Q stops printing before its LAB
        w.print("GCZ6", dt, 9000, 3500.0);
        w.vendor(dt, "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-29"});
    const HeldDay& roll = on(r.value(), "2026-07-29");
    EXPECT_EQ(roll.trigger, RollTrigger::DEADLINE);
    EXPECT_EQ(roll.fallback, RollFallback::NEW_CONTRACT_MOVE);
    EXPECT_FALSE(roll.old_close.has_value());
    EXPECT_EQ(*roll.fallback_new_previous_close, 3500.0);
    ASSERT_EQ(r.value().report.fallback_cases.size(), 1u);
    EXPECT_NE(r.value().report.fallback_cases[0].find("GCQ6"), std::string::npos);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-29 fallback roll_new_move: GCQ6 has no bar up to its "
                     "last actionable bar 2026-07-29; rolled to GCZ6, the old contract's "
                     "move read as the new one's"),
              1u);
}

TEST(ContractRollRule, TheTargetSkipsAMissingCycleFrontWithAWarn) {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        w.print("GCQ6", dt, 80000).print("GCU6", dt, 30000).print("GCG7", dt, 400);
        if (dt != "2026-07-29") w.print("GCZ6", dt, 9000);  // the front has no bar on the roll bar
        w.vendor(dt, "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& roll = on(r.value(), "2026-07-29");
    ASSERT_TRUE(roll.roll);
    EXPECT_EQ(roll.held.raw_symbol, "GCG7");
    EXPECT_TRUE(roll.target_skipped_front);
    EXPECT_EQ(r.value().report.target_skipped_front, 1);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-29 fallback target_not_front: the cycle front after "
                     "GCQ6 has no bar; rolled to GCG7"),
              1u);
}

TEST(ContractRollRule, ANeverListedFrontIsNotCountedAsSkipped) {
    // The cycle front after GCQ6 is GCZ6; when the table never lists it, rolling to GCG7
    // does not skip a listed front and is not flagged.
    World w({{"GCQ6", "q", "2026-08-27"}, {"GCZ6", "z", "2026-12-29"}, {"GCG7", "g", "2027-02-24"}});
    for (const auto& dt : kDates) w.print("GCQ6", dt, 80000).print("GCG7", dt, 400).vendor(dt, "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& roll = on(r.value(), "2026-07-29");
    ASSERT_TRUE(roll.roll);
    EXPECT_EQ(roll.held.raw_symbol, "GCG7");
    EXPECT_FALSE(roll.target_skipped_front);
    EXPECT_EQ(r.value().report.target_skipped_front, 0);
}

TEST(ContractRollRule, NoTargetPrintingHoldsTheRollPending) {
    World w({{"GCQ6", "q", "2026-08-27"}, {"GCZ6", "z", "2026-12-29"}});
    for (const auto& dt : kDates) {
        w.print("GCQ6", dt, 80000);
        if (dt >= "2026-07-31") w.print("GCZ6", dt, 9000);
        w.vendor(dt, "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    // Due on 07-29, no cycle contract prints until 07-31: it rolls there, late.
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-31"});
    EXPECT_TRUE(on(r.value(), "2026-07-31").deferred);
    EXPECT_GE(r.value().report.no_target_bars, 2);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-29 fallback no_target: a roll from GCQ6 is due and "
                     "no cycle contract after it prints"),
              1u);
    EXPECT_EQ(warned(r.value(), "GC.v.0 2026-07-30 fallback no_target"), 1u);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-31 fallback late_deadline: the deadline roll out of "
                     "GCQ6 into GCZ6 falls after its last actionable bar 2026-07-29: no "
                     "target printed until this bar"),
              1u);
}

TEST(ContractRollRule, SeriesStartHoldsTheVendorsCycleContractElseTheCycleFront) {
    {  // The vendor names U (off the cycle): start in the cycle front, Q.
        World w = base_world();
        for (const auto& dt : kDates) w.vendor(dt, "GCU6");
        auto r = w.run();
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().days.front().held.raw_symbol, "GCQ6");
    }
    {  // The vendor names Q past its LAB: start in Z.
        World w = base_world();
        for (const auto& dt : kDates) {
            if (dt >= "2026-07-30") w.vendor(dt, "GCQ6");
        }
        auto r = w.run();
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(r.value().days.front().held.raw_symbol, "GCZ6");
        EXPECT_EQ(r.value().report.rolls, 0);
    }
    {  // Nothing of the cycle prints: refused.
        World w({{"GCU6", "u", "2026-09-28"}});
        w.print("GCU6", "2026-07-15", 1).vendor("2026-07-15", "GCU6");
        EXPECT_TRUE(w.run().is_error());
    }
}

TEST(ContractRollRule, AContractTheRuleNeedsWithNoExpirationIsRefused) {
    World w({{"GCQ6", "q", "2026-08-27"}, {"GCZ6", "z", ""}});
    for (const auto& dt : kDates) w.print("GCQ6", dt, 80000).print("GCZ6", dt, 9000).vendor(dt, "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("GCZ6"), std::string::npos);
}

// What a run whose bars end on a vendor switch can know (HeldDay::awaits_next_bar,
// TentativeRoll). The bar source hands the switch bar out the same way whatever the next bar says.

TEST(ContractRollRule, AVendorSwitchOnItsOwnBarAwaitsTheNextBar) {
    World w = base_world();
    for (const auto& dt : kDates) w.vendor(dt, dt >= "2026-07-21" ? "GCZ6" : "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& roll = on(r.value(), "2026-07-21");
    ASSERT_TRUE(roll.roll);
    EXPECT_TRUE(roll.awaits_next_bar);
    EXPECT_EQ(roll.old_id, "q");
    EXPECT_EQ(*roll.old_close, 1000.0 + 80000 % 97);
    EXPECT_FALSE(roll.tentative.has_value());
    ASSERT_TRUE(roll.old_bar.has_value()) << "the bar a run reads on it: the old contract's own";
    EXPECT_EQ(roll.old_bar->instrument_id, "q");
    EXPECT_EQ(roll.old_bar->close, *roll.old_close);
    // The days after it: no roll, nothing tentative, and the contract left on every one of them.
    EXPECT_FALSE(on(r.value(), "2026-07-22").awaits_next_bar);
    EXPECT_FALSE(on(r.value(), "2026-07-22").tentative.has_value());
    for (const char* dt : {"2026-07-22", "2026-07-23", "2026-08-05"}) {
        ASSERT_TRUE(on(r.value(), dt).left_close.has_value()) << dt;
        EXPECT_EQ(on(r.value(), dt).left_id, "q") << dt;
    }
    EXPECT_FALSE(on(r.value(), "2026-07-21").left_close.has_value()) << "not on the roll bar itself";
}

TEST(ContractRollRule, ASundaySwitchRolledOnMondayIsKnownOnMonday) {
    World w = base_world();
    for (const auto& dt : kDates) w.vendor(dt, dt >= "2026-07-19" ? "GCZ6" : "GCQ6");
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& roll = on(r.value(), "2026-07-20");
    ASSERT_TRUE(roll.roll);
    EXPECT_FALSE(roll.awaits_next_bar);  // Monday's own vendor id confirmed Sunday's switch
    EXPECT_FALSE(on(r.value(), "2026-07-19").tentative.has_value());  // no roll on a Sunday
}

TEST(ContractRollRule, AVendorSwitchOnACrossoverBarIsKnownOnTheBar) {
    // GCQ6's window opens Fri 07-24. On Mon 07-27 the vendor switches to Z and Z out-trades Q: the
    // crossover rolls on the bar whatever the next bar says.
    World w(kAug2026);
    for (const auto& dt : kDates) {
        const bool z_leads = dt >= "2026-07-27";
        w.print("GCQ6", dt, z_leads ? 9000 : 80000).print("GCZ6", dt, z_leads ? 80000 : 9000);
        w.vendor(dt, dt >= "2026-07-27" ? "GCZ6" : "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& roll = on(r.value(), "2026-07-27");
    ASSERT_TRUE(roll.roll);
    EXPECT_EQ(roll.trigger, RollTrigger::VENDOR);
    EXPECT_FALSE(roll.awaits_next_bar);
}

TEST(ContractRollRule, AVendorSwitchOnTheLastActionableBarIsKnownOnTheBar) {
    // GCQ6's LAB is Wed 07-29. The vendor switches to Z there with Q still the volume leader (no
    // crossover): the deadline rolls on the bar whatever the next bar says, so the roll is known on
    // it, not handed out awaiting 07-30 (which would leg it a bar late). Held on 07-30, and the
    // vendor's switch back to Q on 07-30 changes nothing.
    for (const bool back_on_next : {false, true}) {
        SCOPED_TRACE(back_on_next ? "the vendor names Q again on 07-30" : "the vendor keeps Z");
        World w = base_world();
        for (const auto& dt : kDates) {
            std::string v = dt >= "2026-07-29" ? "GCZ6" : "GCQ6";
            if (back_on_next && dt == "2026-07-30") v = "GCQ6";
            w.vendor(dt, v);
        }
        auto r = w.run();
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-29"});
        const HeldDay& roll = on(r.value(), "2026-07-29");
        EXPECT_EQ(roll.held.raw_symbol, "GCZ6");
        EXPECT_FALSE(roll.awaits_next_bar) << "the deadline makes the roll known on its own bar";
        EXPECT_FALSE(roll.tentative.has_value());
        EXPECT_FALSE(roll.late);
        EXPECT_EQ(on(r.value(), "2026-07-30").held.raw_symbol, "GCZ6");
    }
}

TEST(ContractRollRule, AnUnconfirmedSwitchIsTentativeAndRollsNothing) {
    World w = base_world();
    for (const auto& dt : kDates) {
        // a one-bar forward stray on Thu 07-16; the vendor's last grid bar 08-05 names G7, which no
        // later bar can confirm
        std::string v = dt == "2026-07-16" ? "GCZ6" : "GCQ6";
        if (dt >= "2026-07-20") v = "GCZ6";
        if (dt == "2026-08-05") v = "GCG7";
        w.vendor(dt, v);
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldSeries& s = r.value();
    const HeldDay& stray = on(s, "2026-07-16");
    EXPECT_FALSE(stray.roll);
    EXPECT_EQ(stray.held.raw_symbol, "GCQ6");
    ASSERT_TRUE(stray.tentative.has_value());
    EXPECT_EQ(stray.tentative->target.raw_symbol, "GCZ6");
    EXPECT_EQ(stray.tentative->bar.instrument_id, "z");
    EXPECT_EQ(stray.tentative->bar.close, 1000.0 + 9000 % 97);
    EXPECT_EQ(stray.tentative->old_id, "q");
    EXPECT_EQ(stray.tentative->old_close, 1000.0 + 80000 % 97);
    // The switch of 07-20 is confirmed: a roll, awaiting the next bar.
    EXPECT_TRUE(on(s, "2026-07-20").roll);
    EXPECT_TRUE(on(s, "2026-07-20").awaits_next_bar);
    // The last bar's switch to G7 (later than the held Z): unconfirmable, tentative into the
    // cycle front after Z, G7.
    const HeldDay& last = on(s, "2026-08-05");
    EXPECT_FALSE(last.roll);
    ASSERT_TRUE(last.tentative.has_value());
    EXPECT_EQ(last.tentative->target.raw_symbol, "GCG7");
    EXPECT_EQ(last.tentative->old_id, "z");
    EXPECT_EQ(s.report.rolls, 1);
}

TEST(ContractRollRule, ACarriedCloseKeepsTheCarriedRowsId) {
    // GCQ6 has no row on Fri 07-17 while the vendor names Z: carried (class c)
    World w2(kAug2026);
    for (const auto& dt : kDates) {
        if (dt != "2026-07-17") w2.print("GCQ6", dt, 80000);
        w2.print("GCZ6", dt, 9000);
        w2.vendor(dt, dt == "2026-07-17" ? "GCZ6" : "GCQ6");
    }
    auto r = w2.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const HeldDay& carry = on(r.value(), "2026-07-17");
    EXPECT_EQ(carry.source, BarSource::CARRY);
    EXPECT_TRUE(carry.bar_id.empty());
    EXPECT_EQ(carry.carried_id, "q");
    // no tentative on a carry day: the held contract has no bar to roll from
    EXPECT_FALSE(carry.tentative.has_value());
}

// The window ends at the last actionable bar. GCQ6: LAB Wed 07-29, D* Fri 07-31.
// With a bar on the LAB the deadline rolls there first, so the window's end shows only when the
// vendor's grid lacks the LAB's bar: Z out-trades Q on Thu 07-30, after the LAB, and the roll there
// is the deadline's (late), not a crossover. The roll's date is the same either way.
TEST(ContractRollRule, TheWindowEndsAtTheLastActionableBar) {
    World w(kAug2026);
    for (const auto& dt : kDates) {
        const bool z_leads = dt == "2026-07-30";
        w.print("GCQ6", dt, z_leads ? 9000 : 80000).print("GCZ6", dt, z_leads ? 80000 : 9000);
        if (dt != "2026-07-29") w.vendor(dt, "GCQ6");
    }
    auto r = w.run();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(roll_dates(r.value()), std::vector<std::string>{"2026-07-30"});
    EXPECT_EQ(on(r.value(), "2026-07-30").trigger, RollTrigger::DEADLINE);
    EXPECT_TRUE(on(r.value(), "2026-07-30").late);
    EXPECT_EQ(warned(r.value(),
                     "GC.v.0 2026-07-30 fallback late_deadline: the deadline roll out of "
                     "GCQ6 into GCZ6 falls after its last actionable bar 2026-07-29, which "
                     "is not on the vendor's grid: rolled on the next grid bar"),
              1u);
    const ContractDates q6 = contract_dates(on(r.value(), "2026-07-15").held,
                                            contract_rolls_test::shipped_calendar());
    EXPECT_EQ(ymd(q6.window_first), "2026-07-24");
    EXPECT_EQ(ymd(q6.last_actionable), "2026-07-29") << "the window's last day";
}

// The bar source runs the rule from the series start on every load: warnings dated before the
// window are kept in the report, not logged.
TEST(ContractRollRule, WarningsBeforeLogFromAreKeptNotLogged) {
    for (const char* from : {"2026-07-01", "2026-07-30"}) {
        World w(kAug2026);
        for (const auto& dt : kDates) {
            w.print("GCQ6", dt, 80000).print("GCU6", dt, 30000).print("GCG7", dt, 400);
            if (dt != "2026-07-29") w.print("GCZ6", dt, 9000);
            w.vendor(dt, "GCQ6");
        }
        w.log_from(from);
        auto r = w.run();
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        ASSERT_EQ(r.value().report.warnings.size(), 1u) << "the skipped front of 07-29";
        EXPECT_EQ(r.value().report.warnings_not_logged, std::string(from) == "2026-07-30" ? 1 : 0);
    }
}

// A warning on or after log_from is logged only when log_once says so
// (the bar source logs each message once per run, however many loads read its bar); log_once is not asked
// for a warning before log_from. Every warning stays in the report either way.
TEST(ContractRollRule, AWarningIsLoggedOnlyWhenLogOnceAdmitsIt) {
    std::set<std::string> logged;
    std::vector<std::string> asked;
    const auto once = [&](const std::string& m) {
        asked.push_back(m);
        return logged.insert(m).second;
    };
    for (int load = 0; load < 2; ++load) {
        World w(kAug2026);
        for (const auto& dt : kDates) {
            w.print("GCQ6", dt, 80000).print("GCU6", dt, 30000).print("GCG7", dt, 400);
            if (dt != "2026-07-29") w.print("GCZ6", dt, 9000);
            w.vendor(dt, "GCQ6");
        }
        w.log_from("2026-07-01").log_once(once);
        auto r = w.run();
        ASSERT_TRUE(r.is_ok()) << r.error()->what();
        ASSERT_EQ(r.value().report.warnings.size(), 1u) << "kept in the report on every load";
        EXPECT_EQ(r.value().report.warnings_not_logged, load) << "the second load does not log it again";
    }
    ASSERT_EQ(asked.size(), 2u);
    EXPECT_EQ(asked[0], asked[1]);
    // Before log_from, log_once is not asked.
    asked.clear();
    World w(kAug2026);
    for (const auto& dt : kDates) {
        w.print("GCQ6", dt, 80000).print("GCU6", dt, 30000).print("GCG7", dt, 400);
        if (dt != "2026-07-29") w.print("GCZ6", dt, 9000);
        w.vendor(dt, "GCQ6");
    }
    w.log_from("2026-07-30").log_once(once);
    ASSERT_TRUE(w.run().is_ok());
    EXPECT_TRUE(asked.empty());
}

// The held contract's rows end on 07-29 (its bars of 07-30 and
// 07-31 are not loaded yet) while the vendor's grid goes on naming it. Each of those days is the vendor's own
// print with a WARN, as it is when the same days are read from a table that has rows after them: the bar
// does not depend on how far the contract rows reach. No day is left with no contract named and the held
// close carried.
TEST(ContractRollRule, TheVendorsBarStandsInForTheHeldContractsBarsNotLoadedYet) {
    auto world = [](bool later_rows) {
        World w(kAug2026);
        for (const auto& dt : kDates) {
            if (dt > "2026-07-29" && !(later_rows && dt >= "2026-08-03")) continue;
            if (dt == "2026-07-30" || dt == "2026-07-31") continue;  // never loaded on either side
            w.print("GCZ6", dt, 9000, 3400.0 + dt[9]);
        }
        for (const auto& dt : kDates) {
            if (dt > "2026-07-31") break;
            w.vendor(dt, "GCZ6", 5000.0 + dt[9]);
        }
        return w;
    };
    // GCZ6 is held from the start (the vendor names it and it is in the cycle).
    auto only = world(false).run();
    auto later = world(true).run();
    ASSERT_TRUE(only.is_ok()) << only.error()->what();
    ASSERT_TRUE(later.is_ok()) << later.error()->what();
    for (const char* dt : {"2026-07-30", "2026-07-31"}) {
        SCOPED_TRACE(dt);
        const HeldDay& a = on(only.value(), dt);
        const HeldDay& b = on(later.value(), dt);
        EXPECT_EQ(a.held.raw_symbol, "GCZ6");
        EXPECT_EQ(a.source, BarSource::VENDOR) << "the vendor's own print, not a carried close";
        EXPECT_EQ(a.close, 5000.0 + dt[9]);
        EXPECT_EQ(b.source, a.source);
        EXPECT_EQ(b.close, a.close);
        EXPECT_EQ(warned(only.value(), std::string("GC.v.0 ") + dt + " fallback vendor_bar: GCZ6 has no contract "
                                                                     "row and the vendor names it"),
                  1u);
    }
}
