// The per-contract bar source (rolls step 3, per_contract_source.hpp): what each grid date hands
// out, and that no bar depends on a later bar, on hand-built GC fixtures (the shipped calendar), then
// on the clone (TRADE_NGIN_TEST_DSN, read only): the loader's own call with the switch on and off.
// GCQ6: D* = Fri 2026-07-31, window Fri 07-24 to the LAB Wed 07-29.
#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <optional>
#include <tuple>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/data/market_data_bus.hpp"
#include "trade_ngin/data/per_contract_source.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;
using contract_rolls_test::bar;
using contract_rolls_test::d;

namespace {

struct Spec {
    const char* raw;
    const char* id;
    const char* expiration;
};

const std::vector<Spec> kSpecs = {{"GCQ6", "q", "2026-08-27"},
                                  {"GCU6", "u", "2026-09-28"},
                                  {"GCZ6", "z", "2026-12-29"},
                                  {"GCG7", "g", "2027-02-24"}};

const std::vector<std::string> kDates = {
    "2026-07-15", "2026-07-16", "2026-07-17", "2026-07-19", "2026-07-20", "2026-07-21",
    "2026-07-22", "2026-07-23", "2026-07-24", "2026-07-26", "2026-07-27", "2026-07-28",
    "2026-07-29", "2026-07-30", "2026-07-31", "2026-08-02", "2026-08-03", "2026-08-04",
    "2026-08-05"};

/// Contract rows and a vendor grid, run through the rule up to a date (rows and grid both cut there,
/// as build_held_series reads them).
class World {
public:
    World& print(const std::string& raw, const std::string& date, int64_t volume, double close) {
        for (const auto& s : kSpecs) {
            if (raw == s.raw) rows_.push_back(bar("GC", raw, s.id, date, s.expiration, close, volume));
        }
        return *this;
    }
    World& vendor(const std::string& date, const std::string& raw) {
        VendorBar v;
        v.date = d(date);
        v.symbol = "GC.v.0";
        for (const auto& s : kSpecs) {
            if (raw == s.raw) v.instrument_id = s.id;
        }
        v.open = v.high = v.low = v.close = 1.0;
        v.volume = 1;
        grid_.push_back(v);
        return *this;
    }
    HeldSeries run_until(const std::string& last) {
        std::vector<ContractBar> rows;
        for (const auto& r : rows_) {
            if (r.date <= d(last)) rows.push_back(r);
        }
        RuleInputs in;
        in.root = "GC";
        for (const auto& g : grid_) {
            if (g.date <= d(last)) in.grid.push_back(g);
        }
        book_ = std::make_unique<ContractBook>(ContractBook::build(rows).value());
        cal_ = std::make_unique<TradingCalendar>(contract_rolls_test::shipped_calendar("GC"));
        in.book = book_.get();
        in.cal = cal_.get();
        in.cycle = roll_cycle("GC").value();
        auto r = apply_roll_rule(in);
        EXPECT_TRUE(r.is_ok()) << r.error()->what();
        return r.value();
    }
    std::map<Day, SourceBar> bars_until(const std::string& last,
                                        SwitchBar switch_bar = SwitchBar::HELD) {
        const HeldSeries s = run_until(last);
        std::map<Day, std::string> window;
        for (const auto& day : s.days) window[day.date] = ymd(day.date) + " 00:00:00+00";
        auto bars = bars_of_series(s, window, switch_bar);
        EXPECT_TRUE(bars.is_ok()) << bars.error()->what();
        std::map<Day, SourceBar> out;
        for (const auto& b : bars.value()) out[b.date] = b;
        return out;
    }
    /// bars_until with the carry quotes attached (carry on, trade-ngin #185).
    std::map<Day, SourceBar> carry_bars_until(const std::string& last,
                                              SwitchBar switch_bar = SwitchBar::HELD) {
        const HeldSeries s = run_until(last);
        std::map<Day, std::string> window;
        for (const auto& day : s.days) window[day.date] = ymd(day.date) + " 00:00:00+00";
        auto bars = bars_of_series(s, window, switch_bar);
        EXPECT_TRUE(bars.is_ok()) << bars.error()->what();
        std::vector<SourceBar> quoted = bars.value();
        attach_carry_quotes(s, *book_, quoted);
        std::map<Day, SourceBar> out;
        for (const auto& b : quoted) out[b.date] = b;
        return out;
    }

private:
    std::vector<ContractBar> rows_;
    std::vector<VendorBar> grid_;
    std::unique_ptr<ContractBook> book_;
    std::unique_ptr<TradingCalendar> cal_;
};

double close_of(const std::string& raw, const std::string& date) {
    // distinct closes per contract and date, so a bar read from the wrong contract or day shows
    const int day = std::stoi(date.substr(8, 2));
    if (raw == "GCQ6") return 2000.0 + day;
    if (raw == "GCU6") return 2100.0 + day;
    if (raw == "GCZ6") return 2200.0 + day;
    return 2300.0 + day;
}

/// The vendor's shapes: a one-bar forward stray (Thu 07-16), a confirmed switch to Z (Mon 07-20), a
/// two-bar visit back to Q (07-23, 07-24) and a switch on the last bar (08-05) nothing can confirm.
/// U is off the cycle and out-trades Z, so a source that ignored the cycle would read it.
World shapes_world() {
    World w;
    for (const auto& dt : kDates) {
        w.print("GCQ6", dt, 80000, close_of("GCQ6", dt));
        w.print("GCU6", dt, 30000, close_of("GCU6", dt));
        w.print("GCZ6", dt, 9000, close_of("GCZ6", dt));
        w.print("GCG7", dt, 500, close_of("GCG7", dt));
        std::string v = dt >= "2026-07-20" ? "GCZ6" : "GCQ6";
        if (dt == "2026-07-16") v = "GCZ6";
        if (dt == "2026-07-23" || dt == "2026-07-24") v = "GCQ6";
        if (dt == "2026-08-05") v = "GCG7";
        w.vendor(dt, v);
    }
    return w;
}

/// No vendor switch; Z out-trades Q from Mon 07-27, inside GCQ6's window: a crossover.
World crossover_world() {
    World w;
    for (const auto& dt : kDates) {
        const bool z_leads = dt >= "2026-07-27";
        w.print("GCQ6", dt, z_leads ? 9000 : 80000, close_of("GCQ6", dt));
        w.print("GCZ6", dt, z_leads ? 80000 : 9000, close_of("GCZ6", dt));
        w.vendor(dt, "GCQ6");
    }
    return w;
}

bool same(const SourceBar& a, const SourceBar& b) {
    return a.date == b.date && a.symbol == b.symbol && a.open == b.open && a.high == b.high &&
           a.low == b.low && a.close == b.close && a.volume == b.volume &&
           a.instrument_id == b.instrument_id && a.roll_from == b.roll_from &&
           a.time_text == b.time_text;
}

}  // namespace

TEST(PerContractSource, EachGridDateIsTheHeldContractsBarAndARollBarNamesTheContractLeft) {
    auto bars = shapes_world().bars_until("2026-08-05");
    ASSERT_EQ(bars.size(), kDates.size());
    // An ordinary day: the held contract's own bar and id, no roll field.
    const SourceBar& b15 = bars.at(d("2026-07-15"));
    EXPECT_EQ(b15.symbol, "GC.v.0");
    EXPECT_EQ(b15.instrument_id, "q");
    EXPECT_EQ(b15.close, close_of("GCQ6", "2026-07-15"));
    EXPECT_EQ(b15.volume, 80000.0);
    EXPECT_FALSE(b15.roll_from.has_value());
    // The one-bar stray: the held contract's own bar (Q's), the field naming the target Z.
    const SourceBar& b16 = bars.at(d("2026-07-16"));
    EXPECT_EQ(b16.instrument_id, "q");
    EXPECT_EQ(b16.close, close_of("GCQ6", "2026-07-16")) << "every level reads the held contract";
    ASSERT_TRUE(b16.roll_from.has_value());
    EXPECT_EQ(b16.roll_from->close, close_of("GCZ6", "2026-07-16"));
    EXPECT_EQ(b16.roll_from->instrument_id, "z");
    EXPECT_EQ(b16.roll_from->kind, RollFromKind::AWAITS_NEXT);
    EXPECT_EQ(b16.roll_from->date, "2026-07-16") << "rolls step 4: the switch bar's own date";
    // The next bar shows it was not confirmed: Q's own bar.
    EXPECT_EQ(bars.at(d("2026-07-17")).instrument_id, "q");
    EXPECT_FALSE(bars.at(d("2026-07-17")).roll_from.has_value());
    // The confirmed switch of Mon 07-20: the same shape as the stray, the next bar keeps Z.
    const SourceBar& b20 = bars.at(d("2026-07-20"));
    EXPECT_EQ(b20.instrument_id, "q");
    EXPECT_EQ(b20.close, close_of("GCQ6", "2026-07-20"));
    EXPECT_EQ(b20.volume, 80000.0);
    ASSERT_TRUE(b20.roll_from.has_value());
    EXPECT_EQ(b20.roll_from->close, close_of("GCZ6", "2026-07-20"));
    EXPECT_EQ(b20.roll_from->instrument_id, "z");
    EXPECT_EQ(b20.roll_from->kind, RollFromKind::AWAITS_NEXT);
    EXPECT_EQ(b20.roll_from->date, "2026-07-20");
    // Every bar after it, up to the next roll, is Z's own, carrying Q's close of that date for a
    // consumer that withheld the roll bar (and the bars since).
    for (const char* dt : {"2026-07-21", "2026-07-22", "2026-07-23", "2026-08-04"}) {
        const SourceBar& b = bars.at(d(dt));
        EXPECT_EQ(b.instrument_id, "z") << dt;
        EXPECT_EQ(b.close, close_of("GCZ6", dt)) << dt << " (07-23: the vendor's backward visit ignored)";
        ASSERT_TRUE(b.roll_from.has_value()) << dt;
        EXPECT_EQ(b.roll_from->kind, RollFromKind::AFTER_ROLL) << dt;
        EXPECT_EQ(b.roll_from->close, close_of("GCQ6", dt)) << dt;
        EXPECT_EQ(b.roll_from->instrument_id, "q") << dt;
        EXPECT_EQ(b.roll_from->date, "2026-07-20") << dt << ": the roll's date, not the bar's (step 4)";
    }
    // The last bar names G7: unconfirmable, handed out as the switch: Z's own bar, the field G7.
    const SourceBar& last = bars.at(d("2026-08-05"));
    EXPECT_EQ(last.instrument_id, "z");
    EXPECT_EQ(last.close, close_of("GCZ6", "2026-08-05"));
    ASSERT_TRUE(last.roll_from.has_value());
    EXPECT_EQ(last.roll_from->instrument_id, "g");
    EXPECT_EQ(last.roll_from->close, close_of("GCG7", "2026-08-05"));
    EXPECT_EQ(last.roll_from->kind, RollFromKind::AWAITS_NEXT);
}

TEST(PerContractSource, ACrossoverRollIsKnownOnItsOwnBar) {
    auto bars = crossover_world().bars_until("2026-08-05");
    const SourceBar& roll = bars.at(d("2026-07-27"));
    EXPECT_EQ(roll.instrument_id, "z");
    EXPECT_EQ(roll.close, close_of("GCZ6", "2026-07-27"));
    ASSERT_TRUE(roll.roll_from.has_value());
    EXPECT_EQ(roll.roll_from->close, close_of("GCQ6", "2026-07-27"));
    EXPECT_EQ(roll.roll_from->instrument_id, "q");
    EXPECT_EQ(roll.roll_from->kind, RollFromKind::KNOWN);
    EXPECT_EQ(roll.roll_from->date, "2026-07-27");
    EXPECT_EQ(bars.at(d("2026-07-24")).instrument_id, "q");
    EXPECT_EQ(bars.at(d("2026-07-28")).instrument_id, "z");
    ASSERT_TRUE(bars.at(d("2026-07-28")).roll_from.has_value());
    EXPECT_EQ(bars.at(d("2026-07-28")).roll_from->kind, RollFromKind::AFTER_ROLL);
    EXPECT_EQ(bars.at(d("2026-07-28")).roll_from->close, close_of("GCQ6", "2026-07-28"));
    EXPECT_EQ(bars.at(d("2026-07-28")).roll_from->date, "2026-07-27") << "the roll's date (step 4)";
}

// The no-look-ahead property: the bar handed out for a date is the same whatever window ends at or
// after it, so a backtest's single load and a chain of dated runs read the same bars.
TEST(PerContractSource, NoBarDependsOnALaterBar) {
    World shapes = shapes_world();
    World crossover = crossover_world();
    for (World* w : {&shapes, &crossover}) {
        const auto full = w->bars_until(kDates.back());
        for (const auto& last : kDates) {
            const auto prefix = w->bars_until(last);
            ASSERT_EQ(prefix.size(), static_cast<size_t>(std::count_if(
                                         kDates.begin(), kDates.end(),
                                         [&](const std::string& x) { return x <= last; })));
            for (const auto& [date, b] : prefix) {
                EXPECT_TRUE(same(b, full.at(date)))
                    << "the bar of " << ymd(date) << " in the window ending " << last
                    << " differs from the full window's";
            }
        }
    }
}

TEST(PerContractSource, AFallbackRollReadsThePreviousCloseTimesTheNewContractsMove) {
    HeldSeries s;
    s.root = "GC";
    HeldDay a;
    a.date = d("2026-07-28");
    a.held = ContractKey::make("GC", "GCQ6", d("2026-08-27")).value();
    a.bar_id = "q";
    a.open = a.high = a.low = a.close = 2000.0;
    HeldDay b;
    b.date = d("2026-07-29");
    b.held = ContractKey::make("GC", "GCZ6", d("2026-12-29")).value();
    b.bar_id = "z";
    b.open = b.high = b.low = b.close = 2210.0;
    b.roll = true;
    b.old = a.held;
    b.fallback = RollFallback::NEW_CONTRACT_MOVE;
    b.fallback_new_previous_close = 2200.0;
    s.days = {a, b};
    std::map<Day, std::string> window{{a.date, "2026-07-28 00:00:00+00"},
                                      {b.date, "2026-07-29 00:00:00+00"}};
    auto bars = bars_of_series(s, window);
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    const SourceBar& roll = bars.value().back();
    ASSERT_TRUE(roll.roll_from.has_value());
    EXPECT_DOUBLE_EQ(roll.roll_from->close, 2000.0 * 2210.0 / 2200.0);
    EXPECT_EQ(roll.roll_from->instrument_id, "q");
    EXPECT_EQ(roll.roll_from->kind, RollFromKind::KNOWN);
    // A zero fallback: the old contract's close is read as unchanged.
    s.days[1].fallback = RollFallback::ZERO;
    s.days[1].fallback_new_previous_close.reset();
    bars = bars_of_series(s, window);
    ASSERT_TRUE(bars.is_ok());
    EXPECT_EQ(bars.value().back().roll_from->close, 2000.0);
    // Refused: a window date the series lacks, and a carried day with nothing to carry.
    window[d("2026-07-30")] = "2026-07-30 00:00:00+00";
    EXPECT_TRUE(bars_of_series(s, window).is_error());
    window.erase(d("2026-07-30"));
    s.days[0].has_close = false;
    EXPECT_TRUE(bars_of_series(s, window).is_error());
    s.days[0].has_close = true;
    // Refused: a fallback roll on the series' first day (no previous close to read the old
    // contract's from), and a vendor roll awaiting the next bar with no old bar to hand out.
    HeldSeries first;
    first.root = "GC";
    first.days = {s.days[1]};
    auto r = bars_of_series(first, {{b.date, "2026-07-29 00:00:00+00"}});
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("first day"), std::string::npos);
    s.days[1].fallback = RollFallback::NONE;
    s.days[1].old_close = 2005.0;
    s.days[1].old_id = "q";
    s.days[1].awaits_next_bar = true;
    r = bars_of_series(s, window);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("no old bar"), std::string::npos);
}

// R4a open question 4: a fallback roll's WARN is logged from the load's log floor on, once per run: the
// log_once callback is asked only for a roll on or after log_from, and a false keeps the line out.
TEST(PerContractSource, AFallbackRollsWarnIsLoggedFromTheFloorAndOnce) {
    HeldSeries s;
    s.root = "GC";
    HeldDay a;
    a.date = d("2026-07-28");
    a.held = ContractKey::make("GC", "GCQ6", d("2026-08-27")).value();
    a.bar_id = "q";
    a.open = a.high = a.low = a.close = 2000.0;
    HeldDay b = a;
    b.date = d("2026-07-29");
    b.held = ContractKey::make("GC", "GCZ6", d("2026-12-29")).value();
    b.bar_id = "z";
    b.roll = true;
    b.old = a.held;
    b.fallback = RollFallback::ZERO;
    s.days = {a, b};
    const std::map<Day, std::string> window{{a.date, "2026-07-28 00:00:00+00"},
                                            {b.date, "2026-07-29 00:00:00+00"}};
    std::vector<std::string> asked;
    const auto never = [&](const std::string& m) {
        asked.push_back(m);
        return false;
    };
    ASSERT_TRUE(bars_of_series(s, window, SwitchBar::HELD, d("2026-07-30"), never).is_ok());
    EXPECT_TRUE(asked.empty()) << "a roll before the floor is not logged";
    ASSERT_TRUE(bars_of_series(s, window, SwitchBar::HELD, d("2026-07-29"), never).is_ok());
    ASSERT_EQ(asked.size(), 1u);
    EXPECT_EQ(asked[0].rfind("PER_CONTRACT_ROLLS fallback roll GC.v.0 2026-07-29 GCQ6->GCZ6 (zero)", 0), 0u)
        << asked[0];
    // The source's own: the floor and the once-per-run set, both reset by enable() and disable().
    auto& source = PerContractSource::instance();
    source.enable(FuturesCalendar{});
    EXPECT_EQ(source.log_floor(), Day{});
    source.set_log_floor(d("2026-09-14"));
    EXPECT_EQ(source.log_floor(), d("2026-09-14"));
    EXPECT_TRUE(source.first_log("a line"));
    EXPECT_FALSE(source.first_log("a line"));
    EXPECT_TRUE(source.first_log("another line"));
    source.enable(FuturesCalendar{});
    EXPECT_EQ(source.log_floor(), Day{});
    EXPECT_TRUE(source.first_log("a line")) << "a new run logs again";
    source.set_log_floor(d("2026-09-14"));
    source.disable();
    EXPECT_EQ(source.log_floor(), Day{});
    EXPECT_TRUE(source.first_log("another line"));
    source.disable();
}

TEST(PerContractSource, TheSwitchTurnsTheSourceOnAndOff) {
    auto& source = PerContractSource::instance();
    source.disable();
    EXPECT_FALSE(source.enabled());
    source.enable(contract_rolls_test::shipped_calendars());
    EXPECT_TRUE(source.enabled());
    EXPECT_TRUE(source.calendar().for_root("GC").is_ok());
    source.disable();
    EXPECT_FALSE(source.enabled());
    EXPECT_TRUE(configure_per_contract_source(false).is_ok());
    EXPECT_FALSE(source.enabled());
    // Rolls step 4, ruling 19: the switch-bar setting rides with the switch; off reads "held".
    source.enable(contract_rolls_test::shipped_calendars(), SwitchBar::VENDOR);
    EXPECT_EQ(source.switch_bar(), SwitchBar::VENDOR);
    source.disable();
    EXPECT_EQ(source.switch_bar(), SwitchBar::HELD);
    EXPECT_EQ(switch_bar_from("held"), SwitchBar::HELD);
    EXPECT_EQ(switch_bar_from("vendor"), SwitchBar::VENDOR);
    EXPECT_FALSE(switch_bar_from("Vendor").has_value());
    EXPECT_STREQ(to_string(SwitchBar::VENDOR), "vendor");
    EXPECT_STREQ(to_string(SwitchBar::HELD), "held");
}

// ------------------------------------------------------------------------------------------------
// The loader's pieces around the database reads (per_contract_source_bars, source_bars_to_arrow,
// get_futures_instrument_ids, per_contract_bars), without a database.
// ------------------------------------------------------------------------------------------------

namespace {

SourceBar source_bar(const std::string& symbol, const std::string& time_text, double close,
                     const std::string& id, std::optional<RollFrom> roll_from = std::nullopt) {
    SourceBar b;
    b.time_text = time_text;
    b.date = d(time_text.substr(0, 10));
    b.symbol = symbol;
    b.open = close - 1.0;
    b.high = close + 2.0;
    b.low = close - 3.0;
    b.close = close;
    b.volume = 100.0 + close;
    b.instrument_id = id;
    b.roll_from = std::move(roll_from);
    return b;
}

/// One bar of every shape the source hands out: no field, each of the three kinds, no id.
std::vector<SourceBar> every_kind() {
    return {source_bar("GC.v.0", "2026-07-15 00:00:00+00", 2015.0, "q"),
            source_bar("GC.v.0", "2026-07-20 00:00:00+00", 2020.0, "q",
                       RollFrom{2220.0, "z", RollFromKind::AWAITS_NEXT, "2026-07-20"}),
            source_bar("GC.v.0", "2026-07-27 00:00:00+00", 2227.0, "z",
                       RollFrom{2027.0, "q", RollFromKind::KNOWN, "2026-07-27"}),
            source_bar("GC.v.0", "2026-07-28 00:00:00+00", 2228.0, "z",
                       RollFrom{2028.0, "q", RollFromKind::AFTER_ROLL, "2026-07-27"}),
            source_bar("MES.v.0", "2026-07-28 00:00:00+00", 6000.0, ""),
            // R6: the micros' change of price source, and a bar after it (MM fix round 1: each
            // carrying the full-size contract's own volume)
            source_bar("MES.v.0", "2026-02-23 00:00:00+00", 6857.25, "m",
                       RollFrom{6857.25, "e", RollFromKind::SOURCE_CHANGE, "2026-02-23", 1250000.0}),
            source_bar("MES.v.0", "2026-02-24 00:00:00+00", 6904.25, "m",
                       RollFrom{6904.25, "e", RollFromKind::AFTER_SOURCE_CHANGE, "2026-02-23", 980000.0}),
            // MM fix round 1: an equity micro's bar before its change, its own contract's volume along
            [] {
                SourceBar b = source_bar("MES.v.0", "2019-05-06 00:00:00+00", 2918.0, "736");
                b.own_contract_volume = 162154.0;
                return b;
            }()};
}

/// `table` with column `name` replaced by `column`.
std::shared_ptr<arrow::Table> with_column(const std::shared_ptr<arrow::Table>& table,
                                          const std::string& name,
                                          const std::shared_ptr<arrow::Array>& column) {
    const int i = table->schema()->GetFieldIndex(name);
    return table
        ->SetColumn(i, arrow::field(name, column->type()),
                    std::make_shared<arrow::ChunkedArray>(column))
        .ValueOrDie();
}

}  // namespace

// R6: the rule serves every root asked for, the four micros too (no root is left on the vendor's
// series); any other symbol is refused.
TEST(PerContractSourceLoader, EveryRootIsTheRulesTheMicrosIncludedAndAnyOtherSymbolIsRefused) {
    auto roots = source_roots({"GC.v.0", "MES.v.0", "ZN.v.0", "M2K.v.0", "MNQ.v.0", "MYM.v.0"});
    ASSERT_TRUE(roots.is_ok()) << roots.error()->what();
    EXPECT_EQ(roots.value(), (std::vector<std::string>{"GC", "MES", "ZN", "M2K", "MNQ", "MYM"}));
    EXPECT_TRUE(source_roots({}).value().empty());
    for (const char* bad : {"GC", "GC.v.1", ".v.0", "GCQ6"}) {
        auto r = source_roots({"GC.v.0", bad});
        ASSERT_TRUE(r.is_error()) << bad;
        EXPECT_NE(std::string(r.error()->what()).find(bad), std::string::npos) << bad;
    }
}

TEST(PerContractSourceLoader, TheBarsSortAsTheQueryAndTheIdRowsBySymbolThenDate) {
    std::vector<SourceBar> bars = {source_bar("ZN.v.0", "2026-07-16 00:00:00+00", 1.0, "n2"),
                                   source_bar("GC.v.0", "2026-07-16 00:00:00+00", 2.0, "q2"),
                                   source_bar("ZN.v.0", "2026-07-15 00:00:00+00", 3.0, "n1"),
                                   source_bar("MES.v.0", "2026-07-15 00:00:00+00", 4.0, ""),
                                   source_bar("GC.v.0", "2026-07-15 00:00:00+00", 5.0, "q1")};
    sort_source_bars(bars);
    std::vector<std::string> order;
    for (const auto& b : bars) order.push_back(b.time_text.substr(8, 2) + " " + b.symbol);
    EXPECT_EQ(order, (std::vector<std::string>{"15 GC.v.0", "15 MES.v.0", "15 ZN.v.0", "16 GC.v.0",
                                               "16 ZN.v.0"}));
    const auto rows = instrument_id_rows(bars);
    ASSERT_EQ(rows.size(), 4u) << "a bar with no id has no row, as the inner join drops it";
    EXPECT_EQ(rows[0].symbol, "GC.v.0");
    EXPECT_EQ(rows[0].date, "2026-07-15");
    EXPECT_EQ(rows[0].instrument_id, "q1");
    EXPECT_EQ(rows[1].instrument_id, "q2");
    EXPECT_EQ(rows[2].symbol, "ZN.v.0");
    EXPECT_EQ(rows[2].instrument_id, "n1");
    EXPECT_EQ(rows[3].date, "2026-07-16");
}

// The loader's table carries the field through the conversion every reader uses
// (DataConversionUtils::arrow_table_to_bars): each kind arrives as itself.
TEST(PerContractSourceLoader, TheArrowTableRoundTripsEveryKindOfTheField) {
    const std::vector<SourceBar> source = every_kind();
    auto table = source_bars_to_arrow(source);
    ASSERT_TRUE(table.is_ok()) << table.error()->what();
    EXPECT_EQ(table.value()->num_columns(), 14);
    EXPECT_EQ(table.value()->num_rows(), static_cast<int64_t>(source.size()));
    auto bars = DataConversionUtils::arrow_table_to_bars(table.value());
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    ASSERT_EQ(bars.value().size(), source.size());
    for (size_t i = 0; i < source.size(); ++i) {
        const Bar& b = bars.value()[i];
        const SourceBar& s = source[i];
        SCOPED_TRACE(s.time_text + " " + s.symbol);
        EXPECT_EQ(b.symbol, s.symbol);
        EXPECT_EQ(b.timestamp, source_bar_time(s.time_text));
        EXPECT_EQ(core::format_utc_date(b.timestamp), s.time_text.substr(0, 10));
        EXPECT_EQ(static_cast<double>(b.open), s.open);
        EXPECT_EQ(static_cast<double>(b.high), s.high);
        EXPECT_EQ(static_cast<double>(b.low), s.low);
        EXPECT_EQ(static_cast<double>(b.close), s.close);
        EXPECT_EQ(b.volume, s.volume);
        EXPECT_EQ(b.instrument_id, s.instrument_id);
        EXPECT_EQ(b.roll_from, s.roll_from);
        EXPECT_EQ(b.own_contract_volume, s.own_contract_volume);
    }
    EXPECT_EQ(bars.value()[7].own_contract_volume, std::optional<double>(162154.0));
    EXPECT_FALSE(bars.value()[5].own_contract_volume.has_value());
    EXPECT_EQ(bars.value()[1].roll_from->kind, RollFromKind::AWAITS_NEXT);
    EXPECT_EQ(bars.value()[2].roll_from->kind, RollFromKind::KNOWN);
    EXPECT_EQ(bars.value()[3].roll_from->kind, RollFromKind::AFTER_ROLL);
    EXPECT_EQ(bars.value()[3].roll_from->date, "2026-07-27") << "rolls step 4: the roll's date";
    EXPECT_EQ(bars.value()[5].roll_from->kind, RollFromKind::SOURCE_CHANGE);
    EXPECT_EQ(bars.value()[6].roll_from->kind, RollFromKind::AFTER_SOURCE_CHANGE);
    // MM fix round 1: the full-size contract's volume on the two change kinds, none elsewhere
    EXPECT_EQ(bars.value()[5].roll_from->volume, std::optional<double>(1250000.0));
    EXPECT_EQ(bars.value()[6].roll_from->volume, std::optional<double>(980000.0));
    EXPECT_FALSE(bars.value()[2].roll_from->volume.has_value());
    // An empty source is an empty table of the same fourteen columns.
    auto empty = source_bars_to_arrow({});
    ASSERT_TRUE(empty.is_ok());
    EXPECT_EQ(empty.value()->num_columns(), 14);
    // A table without the volume column (a field before MM's fix round) reads every field with
    // no volume.
    auto no_volume = table.value()
                         ->RemoveColumn(table.value()->schema()->GetFieldIndex("roll_from_volume"))
                         .ValueOrDie();
    auto unvolumed = DataConversionUtils::arrow_table_to_bars(no_volume);
    ASSERT_TRUE(unvolumed.is_ok()) << unvolumed.error()->what();
    EXPECT_FALSE(unvolumed.value()[5].roll_from->volume.has_value());
    EXPECT_EQ(unvolumed.value()[5].roll_from->close, 6857.25);
    // A table without the date column (a field of step 3) reads every field with no date.
    auto no_date = table.value()
                       ->RemoveColumn(table.value()->schema()->GetFieldIndex("roll_from_date"))
                       .ValueOrDie();
    auto undated = DataConversionUtils::arrow_table_to_bars(no_date);
    ASSERT_TRUE(undated.is_ok()) << undated.error()->what();
    EXPECT_EQ(undated.value()[3].roll_from->date, "");
    EXPECT_EQ(undated.value()[3].roll_from->close, 2028.0);
    EXPECT_EQ(empty.value()->num_rows(), 0);
}

TEST(PerContractSourceLoader, AMalformedRollFieldIsRefusedNotReadAsAnotherKind) {
    const auto table = source_bars_to_arrow(every_kind()).value();
    // the close without the id and kind columns
    auto no_id = table->RemoveColumn(table->schema()->GetFieldIndex("roll_from_id")).ValueOrDie();
    EXPECT_TRUE(DataConversionUtils::arrow_table_to_bars(no_id).is_error());
    // a kind past the last one, and a NULL kind beside a close
    for (const bool null_kind : {false, true}) {
        arrow::Int8Builder kinds;
        for (int64_t i = 0; i < table->num_rows(); ++i) {
            if (i == 2) {
                // 6: past AFTER_SOURCE_CHANGE (5), the last kind (R6)
                ASSERT_TRUE((null_kind ? kinds.AppendNull() : kinds.Append(6)).ok());
            } else {
                ASSERT_TRUE(kinds.Append(0).ok());
            }
        }
        std::shared_ptr<arrow::Array> column;
        ASSERT_TRUE(kinds.Finish(&column).ok());
        auto r =
            DataConversionUtils::arrow_table_to_bars(with_column(table, "roll_from_kind", column));
        ASSERT_TRUE(r.is_error()) << (null_kind ? "NULL kind" : "kind 6");
    }
}

TEST(PerContractSourceLoader, TheBusEventIsTheBarsOhlcvAtItsInstant) {
    const SourceBar b = every_kind()[2];
    const MarketDataEvent e = source_bar_event(b);
    EXPECT_EQ(e.type, MarketDataEventType::BAR);
    EXPECT_EQ(e.symbol, "GC.v.0");
    EXPECT_EQ(e.timestamp, source_bar_time(b.time_text));
    EXPECT_EQ(core::format_utc_date(e.timestamp), "2026-07-27");
    EXPECT_EQ(e.numeric_fields.at("open"), b.open);
    EXPECT_EQ(e.numeric_fields.at("high"), b.high);
    EXPECT_EQ(e.numeric_fields.at("low"), b.low);
    EXPECT_EQ(e.numeric_fields.at("close"), b.close);
    EXPECT_EQ(e.numeric_fields.at("volume"), b.volume);
}

TEST(PerContractSourceLoader, TheGridWindowKeysEachDateOnceAndDropsTheDaysBeforeTheSeriesStart) {
    auto w = grid_window("GC", {{"2026-07-15 00:00:00+00", "2026-07-15"},
                                {"2026-07-16 00:00:00+00", "2026-07-16"},
                                {"2026-07-17 00:00:00+00", "2026-07-17"}});
    ASSERT_TRUE(w.is_ok()) << w.error()->what();
    ASSERT_EQ(w.value().size(), 3u);
    EXPECT_EQ(w.value().at(d("2026-07-16")), "2026-07-16 00:00:00+00");
    std::map<Day, std::string> window = w.value();
    EXPECT_EQ(drop_before_series_start("GC", window, d("2026-07-15")), 0u);
    EXPECT_EQ(window.size(), 3u);
    EXPECT_EQ(drop_before_series_start("GC", window, d("2026-07-17")), 2u);
    ASSERT_EQ(window.size(), 1u);
    EXPECT_EQ(window.begin()->first, d("2026-07-17"));
    EXPECT_EQ(drop_before_series_start("GC", window, d("2026-08-01")), 1u);
    EXPECT_TRUE(window.empty());
    EXPECT_TRUE(grid_window("GC", {}).value().empty());
    auto twice = grid_window("GC", {{"2026-07-15 00:00:00+00", "2026-07-15"},
                                    {"2026-07-15 17:00:00+00", "2026-07-15"}});
    ASSERT_TRUE(twice.is_error());
    EXPECT_NE(std::string(twice.error()->what()).find("two instants on 2026-07-15"),
              std::string::npos);
}

TEST(PerContractSourceLoader, TheSwitchOnLoadsTheCalendarAndRefusesWithoutOne) {
    const char* before = std::getenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON");
    const std::string saved = before ? before : "";
    setenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON",
           contract_rolls_test::shipped_futures_calendar_path().c_str(), 1);
    EXPECT_TRUE(configure_per_contract_source(true).is_ok());
    EXPECT_TRUE(PerContractSource::instance().enabled());
    EXPECT_TRUE(PerContractSource::instance().calendar().for_root("GC").is_ok());
    EXPECT_EQ(PerContractSource::instance().switch_bar(), SwitchBar::HELD);
    EXPECT_TRUE(configure_per_contract_source(true, "vendor").is_ok());
    EXPECT_EQ(PerContractSource::instance().switch_bar(), SwitchBar::VENDOR);
    auto bad = configure_per_contract_source(true, "target");
    ASSERT_TRUE(bad.is_error());
    EXPECT_NE(std::string(bad.error()->what()).find("per_contract_switch_bar"), std::string::npos);
    EXPECT_FALSE(PerContractSource::instance().enabled());
    setenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON", "/nonexistent/futures_holidays.json", 1);
    PerContractSource::instance().disable();
    auto r = configure_per_contract_source(true);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("cannot be loaded"), std::string::npos);
    EXPECT_FALSE(PerContractSource::instance().enabled());
    if (before) {
        setenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON", saved.c_str(), 1);
    } else {
        unsetenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON");
    }
    PerContractSource::instance().disable();
}

// ------------------------------------------------------------------------------------------------
// On the clone: PostgresDatabase::get_market_data with the switch on and off.
// ------------------------------------------------------------------------------------------------

namespace {

class PerContractSourceDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string dsn = contract_rolls_test::dsn();
        if (dsn.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        db_ = std::make_shared<PostgresDatabase>(dsn);
        if (db_->connect().is_error() || !db_->is_connected()) GTEST_SKIP() << "database unreachable";
        pqxx::connection c(dsn);
        pqxx::read_transaction t(c);
        if (t.exec("SELECT count(*) FROM futures_data.contract_ohlcv_1d WHERE root = 'GC' AND "
                   "expiration IS NULL")[0][0]
                .as<int64_t>() > 0) {
            GTEST_SKIP() << "the clone's expiration is not filled (fill_expiration.py)";
        }
    }
    void TearDown() override {
        PerContractSource::instance().disable();
        if (db_ && db_->is_connected()) db_->disconnect();
    }
    std::vector<Bar> load(const std::vector<std::string>& symbols, const std::string& from,
                          const std::string& to) {
        auto ts = [](const std::string& s) {
            return std::chrono::system_clock::time_point(std::chrono::sys_days(d(s)));
        };
        auto table = db_->get_market_data(symbols, ts(from), ts(to), AssetClass::FUTURES);
        EXPECT_TRUE(table.is_ok()) << table.error()->what();
        auto bars = DataConversionUtils::arrow_table_to_bars(table.value());
        EXPECT_TRUE(bars.is_ok()) << bars.error()->what();
        return bars.value();
    }
    std::shared_ptr<PostgresDatabase> db_;
};

std::string key_of(const Bar& b) {
    return b.symbol + "|" + std::to_string(std::chrono::system_clock::to_time_t(b.timestamp));
}

std::map<std::string, const Bar*> by_key(const std::vector<Bar>& bars) {
    std::map<std::string, const Bar*> out;
    for (const auto& b : bars) out[key_of(b)] = &b;
    return out;
}

}  // namespace

TEST_F(PerContractSourceDbTest, TheGcExampleOnTheLoadersOwnCall) {
    PerContractSource::instance().disable();
    const auto vendor = load({"GC.v.0", "MES.v.0"}, "2026-07-01", "2026-08-31");
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars());
    const auto ours = load({"GC.v.0", "MES.v.0"}, "2026-07-01", "2026-08-31");
    // The grid stays the vendor's: the same symbols and instants.
    const auto v = by_key(vendor);
    const auto o = by_key(ours);
    ASSERT_EQ(v.size(), o.size());
    for (const auto& [key, bar] : v) ASSERT_TRUE(o.count(key)) << key;
    for (const auto& b : vendor) EXPECT_FALSE(b.roll_from.has_value());
    int rolls = 0;
    for (const auto& b : ours) {
        if (b.symbol == "MES.v.0") {
            // R6: MES is read by the rule too; it holds MESU6 all window, the vendor's contract
            const Bar* vb = v.at(key_of(b));
            EXPECT_EQ(b.close, vb->close);
            EXPECT_EQ(b.instrument_id, vb->instrument_id);
            EXPECT_FALSE(b.roll_from.has_value());
            continue;
        }
        if (!b.roll_from) continue;
        // a vendor switch only the next bar confirms is handed out awaiting it, and the bar after a
        // roll carries the contract left
        if (b.roll_from->kind != RollFromKind::KNOWN) continue;
        ++rolls;
        // R2's GC example: GCQ6 to GCZ6 on the crossover of Wed 2026-07-29, known on the bar
        EXPECT_EQ(core::format_utc_date(b.timestamp), "2026-07-29");
        EXPECT_NE(b.roll_from->instrument_id, b.instrument_id);
    }
    EXPECT_EQ(rolls, 1);
    // A window that starts later reads the same bars on the dates both hold.
    const auto later_bars = load({"GC.v.0"}, "2026-07-25", "2026-08-31");
    const auto later = by_key(later_bars);
    for (const auto& [key, bar] : later) {
        const Bar* full = o.at(key);
        EXPECT_EQ(bar->close, full->close) << key;
        EXPECT_EQ(bar->instrument_id, full->instrument_id) << key;
        EXPECT_EQ(bar->roll_from.has_value(), full->roll_from.has_value()) << key;
    }
    // The classifier's id rows are the bars' ids.
    const auto ts = [](const std::string& s) {
        return std::chrono::system_clock::time_point(std::chrono::sys_days(d(s)));
    };
    auto ids = db_->get_futures_instrument_ids({"GC.v.0"}, ts("2026-07-01"), ts("2026-08-31"));
    ASSERT_TRUE(ids.is_ok()) << ids.error()->what();
    size_t gc_bars = 0;
    for (const auto& b : ours) gc_bars += b.symbol == "GC.v.0";
    ASSERT_EQ(ids.value().size(), gc_bars);
    for (const auto& row : ids.value()) {
        bool found = false;
        for (const auto& b : ours) {
            if (b.symbol == row.symbol && core::format_utc_date(b.timestamp) == row.date) {
                EXPECT_EQ(b.instrument_id, row.instrument_id) << row.date;
                found = true;
            }
        }
        EXPECT_TRUE(found) << row.date;
    }
}

// Rolls step 4, ruling 19: with per_contract_switch_bar "vendor" each switch bar (the one-bar stray of
// 07-16, the confirmed switch of 07-20, the unconfirmable switch of 08-05) is the TARGET's own bar,
// its close, id and volume, the field naming the held contract; every other bar is the "held" one.
TEST(PerContractSource, TheVendorSettingHandsOutTheTargetsBarOnEverySwitchBarAndNothingElse) {
    auto held = shapes_world().bars_until("2026-08-05");
    auto vendor = shapes_world().bars_until("2026-08-05", SwitchBar::VENDOR);
    ASSERT_EQ(held.size(), vendor.size());
    const std::map<std::string, std::pair<std::string, std::string>> switches = {
        {"2026-07-16", {"GCZ6", "z"}}, {"2026-07-20", {"GCZ6", "z"}}, {"2026-08-05", {"GCG7", "g"}}};
    for (const auto& [day, h] : held) {
        const SourceBar& v = vendor.at(day);
        const auto sw = switches.find(ymd(day));
        if (sw == switches.end()) {
            EXPECT_EQ(v.close, h.close) << ymd(day);
            EXPECT_EQ(v.instrument_id, h.instrument_id) << ymd(day);
            EXPECT_EQ(v.volume, h.volume) << ymd(day);
            EXPECT_EQ(v.roll_from, h.roll_from) << ymd(day);
            continue;
        }
        SCOPED_TRACE(ymd(day));
        const auto& [target, target_id] = sw->second;
        EXPECT_EQ(v.close, close_of(target, ymd(day)));
        EXPECT_EQ(v.instrument_id, target_id);
        EXPECT_EQ(v.volume, target == "GCZ6" ? 9000.0 : 500.0);
        ASSERT_TRUE(v.roll_from.has_value());
        EXPECT_EQ(v.roll_from->kind, RollFromKind::AWAITS_NEXT_VENDOR);
        EXPECT_EQ(v.roll_from->close, h.close) << "the field names the held contract's close";
        EXPECT_EQ(v.roll_from->instrument_id, h.instrument_id);
        EXPECT_EQ(v.roll_from->date, ymd(day));
        // the "held" bar is the mirror image: the held contract's bar, the field naming the target
        EXPECT_EQ(h.roll_from->kind, RollFromKind::AWAITS_NEXT);
        EXPECT_EQ(h.roll_from->close, v.close);
        EXPECT_EQ(h.roll_from->instrument_id, v.instrument_id);
    }
    // The Arrow table carries the new kind as itself.
    std::vector<SourceBar> rows;
    for (const auto& [day, b] : vendor) rows.push_back(b);
    auto table = source_bars_to_arrow(rows);
    ASSERT_TRUE(table.is_ok());
    auto bars = DataConversionUtils::arrow_table_to_bars(table.value());
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    size_t i = 0;
    for (const auto& [day, b] : vendor) EXPECT_EQ(bars.value()[i++].roll_from, b.roll_from) << ymd(day);
}

// The library reads the two settings' bars to the same booked moves, legs, held contracts, returns and
// adjusted level differences; only the raw close of each switch bar differs (the level readers' price).
TEST(PerContractSource, BothSwitchBarSettingsBookTheSameMovesAndLegs) {
    auto held = shapes_world().bars_until("2026-08-05");
    auto vendor = shapes_world().bars_until("2026-08-05", SwitchBar::VENDOR);
    auto walk = [](const std::map<Day, SourceBar>& bars) {
        std::vector<double> raw;
        std::vector<std::string> ids;
        roll_series::RollFields fields;
        for (const auto& [day, b] : bars) {
            raw.push_back(b.close);
            ids.push_back(b.instrument_id);
            fields.push_back(b.roll_from);
        }
        return std::make_tuple(raw, ids, fields);
    };
    const auto [raw_h, ids_h, f_h] = walk(held);
    const auto [raw_v, ids_v, f_v] = walk(vendor);
    const auto sh = roll_series::build_series(raw_h, ids_h, f_h);
    const auto sv = roll_series::build_series(raw_v, ids_v, f_v);
    ASSERT_EQ(sh.returns.size(), sv.returns.size());
    for (size_t t = 0; t < sh.returns.size(); ++t) EXPECT_NEAR(sh.returns[t], sv.returns[t], 1e-15) << t;
    // The levels differ by one constant: a series ending on an unconfirmed switch bar (08-05) is
    // anchored on that bar's raw close, the target's under "vendor". Every difference (what the EMAs
    // and the returns read) is the same.
    for (size_t t = 1; t < sh.adjusted.size(); ++t) {
        EXPECT_NEAR(sh.adjusted[t] - sh.adjusted[t - 1], sv.adjusted[t] - sv.adjusted[t - 1], 1e-9) << t;
    }
    EXPECT_EQ(sh.flags.held_id, sv.flags.held_id);
    EXPECT_EQ(sh.flags.confirm, sv.flags.confirm);
    EXPECT_EQ(sh.flags.pending, sv.flags.pending);
    roll_series::RollTracker th, tv;
    int differing_raw = 0;
    for (size_t t = 0; t < raw_h.size(); ++t) {
        SCOPED_TRACE(t);
        const auto a = th.add(ids_h[t], raw_h[t], f_h[t]);
        const auto b = tv.add(ids_v[t], raw_v[t], f_v[t]);
        if (t > 0) EXPECT_DOUBLE_EQ(a.move(raw_h[t], raw_h[t - 1]), b.move(raw_v[t], raw_v[t - 1]));
        EXPECT_EQ(a.holds(), b.holds());
        EXPECT_EQ(a.confirm, b.confirm);
        EXPECT_EQ(a.held_id, b.held_id);
        if (a.confirm) {
            EXPECT_EQ(a.last_close_before_change, b.last_close_before_change) << "the closing leg";
            EXPECT_EQ(a.change_bar_close, b.change_bar_close) << "the opening leg";
            EXPECT_EQ(a.previous_held_id, b.previous_held_id);
        }
        differing_raw += raw_h[t] != raw_v[t];
    }
    EXPECT_EQ(differing_raw, 3) << "only the three switch bars' closes";
}

// R4A fix round 1 (audit A S1): the setting reaches the bar query. CL's vendor switch of 2025-01-15 (CLG5, id 615689,
// to CLH5, id 37014) through get_market_data, the loader's own call: "held" hands out CLG5's bar with the field naming
// CLH5; "vendor" hands out CLH5's bar with the field naming CLG5. Every other bar of the window is the same.
TEST_F(PerContractSourceDbTest, TheSwitchBarSettingReachesTheBarQuery) {
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars(), SwitchBar::HELD);
    const auto held = load({"CL.v.0"}, "2025-01-08", "2025-01-22");
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars(), SwitchBar::VENDOR);
    const auto vendor = load({"CL.v.0"}, "2025-01-08", "2025-01-22");
    ASSERT_EQ(held.size(), vendor.size());
    ASSERT_FALSE(held.empty());
    int switch_bars = 0;
    for (size_t i = 0; i < held.size(); ++i) {
        const Bar& h = held[i];
        const Bar& v = vendor[i];
        const std::string day = core::format_utc_date(h.timestamp);
        ASSERT_EQ(day, core::format_utc_date(v.timestamp));
        if (day != "2025-01-15") {
            EXPECT_EQ(h.close, v.close) << day;
            EXPECT_EQ(h.instrument_id, v.instrument_id) << day;
            EXPECT_EQ(h.roll_from, v.roll_from) << day;
            continue;
        }
        ++switch_bars;
        ASSERT_TRUE(h.roll_from.has_value());
        ASSERT_TRUE(v.roll_from.has_value());
        EXPECT_EQ(h.instrument_id, "615689") << "held: CLG5's own bar";
        EXPECT_NEAR(static_cast<double>(h.close), 80.38, 1e-9);
        EXPECT_EQ(h.roll_from->kind, RollFromKind::AWAITS_NEXT);
        EXPECT_EQ(h.roll_from->instrument_id, "37014");
        EXPECT_NEAR(h.roll_from->close, 79.06, 1e-9);
        EXPECT_EQ(v.instrument_id, "37014") << "vendor: CLH5's own bar";
        EXPECT_NEAR(static_cast<double>(v.close), 79.06, 1e-9);
        EXPECT_EQ(v.roll_from->kind, RollFromKind::AWAITS_NEXT_VENDOR);
        EXPECT_EQ(v.roll_from->instrument_id, "615689");
        EXPECT_NEAR(v.roll_from->close, 80.38, 1e-9);
        EXPECT_EQ(v.roll_from->date, "2025-01-15");
    }
    EXPECT_EQ(switch_bars, 1);
}

// MM (HD 2026-10-27): the gold and silver micros through the loader's own call, on the loaded copy.
// Their own .v.0 starts at the listing; before it the series is the full-size contract the rule holds,
// on the full-size root's grid (GC.v.0's Friday 2010-10-01 and Sunday 10-03), and starts where the
// full-size series does. The listing bar is the one change of price source: MGC GCZ0 to MGCZ0 (same
// expiry) on Monday 2010-10-04, SIL SIN3 to SILU3 (SIL lists no July) on Monday 2013-06-17.
TEST_F(PerContractSourceDbTest, TheGoldAndSilverMicrosReadTheFullSizeRootUntilTheirListing) {
    {
        pqxx::connection c(contract_rolls_test::dsn());
        pqxx::read_transaction t(c);
        if (t.exec("SELECT count(*) FROM futures_data.ohlcv_1d WHERE symbol IN ('MGC.v.0', 'SIL.v.0')")[0][0]
                .as<int64_t>() == 0) {
            GTEST_SKIP() << "MGC and SIL are not loaded on this database";
        }
        for (const auto& [root, full] : {std::pair<std::string, std::string>{"MGC", "GC"}, {"SIL", "SI"}}) {
            auto start = series_start(t, root);
            ASSERT_TRUE(start.is_ok()) << start.error()->what();
            auto full_start = series_start(t, full);
            ASSERT_TRUE(full_start.is_ok()) << full_start.error()->what();
            EXPECT_EQ(ymd(start.value()), ymd(full_start.value())) << root;
            EXPECT_EQ(ymd(start.value()), "2010-06-07") << root;
        }
    }
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars());
    const auto gold = load({"MGC.v.0"}, "2010-09-27", "2010-10-08");
    std::map<std::string, const Bar*> on;
    for (const auto& b : gold) on[core::format_utc_date(b.timestamp)] = &b;
    ASSERT_EQ(on.size(), 11u) << "the grid's 09-27 to 10-01 and 10-03 are GC.v.0's, 10-04 to 10-08 MGC.v.0's";
    for (const char* day : {"2010-09-27", "2010-10-01", "2010-10-03"}) {
        ASSERT_TRUE(on.count(day)) << day;
        EXPECT_EQ(on.at(day)->instrument_id, "197012") << day << ": GCZ0";
        EXPECT_FALSE(on.at(day)->roll_from.has_value()) << day;
    }
    EXPECT_DOUBLE_EQ(static_cast<double>(on.at("2010-10-01")->close), 1320.0) << "GCZ0's own close";
    const Bar& change = *on.at("2010-10-04");
    EXPECT_EQ(change.instrument_id, "88890") << "MGCZ0";
    EXPECT_DOUBLE_EQ(static_cast<double>(change.close), 1315.8);
    ASSERT_TRUE(change.roll_from.has_value());
    EXPECT_EQ(change.roll_from->kind, RollFromKind::SOURCE_CHANGE);
    EXPECT_EQ(change.roll_from->instrument_id, "197012");
    EXPECT_DOUBLE_EQ(change.roll_from->close, 1315.8);
    EXPECT_EQ(on.at("2010-10-05")->roll_from->kind, RollFromKind::AFTER_SOURCE_CHANGE);

    const auto silver = load({"SIL.v.0"}, "2013-06-10", "2013-06-21");
    on.clear();
    for (const auto& b : silver) on[core::format_utc_date(b.timestamp)] = &b;
    for (const char* day : {"2013-06-10", "2013-06-14", "2013-06-16"}) {
        ASSERT_TRUE(on.count(day)) << day;
        EXPECT_EQ(on.at(day)->instrument_id, "113863") << day << ": SIN3";
    }
    const Bar& listing = *on.at("2013-06-17");
    EXPECT_EQ(listing.instrument_id, "131488") << "SILU3";
    EXPECT_DOUBLE_EQ(static_cast<double>(listing.close), 21.8);
    ASSERT_TRUE(listing.roll_from.has_value());
    EXPECT_EQ(listing.roll_from->kind, RollFromKind::SOURCE_CHANGE);
    EXPECT_EQ(listing.roll_from->instrument_id, "113863");
    EXPECT_DOUBLE_EQ(listing.roll_from->close, 21.83);
    PerContractSource::instance().disable();
}

// R6 (plan 10b point 5): the micros read the rule through the loader's own call. Before the
// vendor's 2026-02-22 change MES.v.0 is the ES contract the rule holds, on Sunday 02-22 too (ESH6's
// own bar, where the vendor names MESH6); Monday 02-23 is the one roll into MESH6, known on its bar,
// the field ESH6's close and id; after it the field carries ESH6 while it prints. In the listing-date
// era (2019) the bars are ES contracts, the vendor's. The series starts where the vendor's does.
TEST_F(PerContractSourceDbTest, TheMicrosReadTheFullSizeContractsUntilTheChangeAndRollOnceAcrossIt) {
    {
        pqxx::connection c(contract_rolls_test::dsn());
        pqxx::read_transaction t(c);
        if (t.exec("SELECT count(*) FROM futures_data.contract_ohlcv_1d WHERE root = 'ES' AND "
                   "date = '2026-02-23'")[0][0]
                .as<int64_t>() == 0) {
            GTEST_SKIP() << "ES contract bars are not loaded on this database";
        }
        for (const auto& [root, first] : {std::pair<std::string, std::string>{"MES", "2010-06-07"},
                                          {"M2K", "2017-07-09"}}) {
            auto start = series_start(t, root);
            ASSERT_TRUE(start.is_ok()) << start.error()->what();
            const auto vendor_first = t.exec("SELECT min((time AT TIME ZONE 'UTC')::date)::text FROM "
                                             "futures_data.ohlcv_1d WHERE symbol = $1",
                                             pqxx::params{root + ".v.0"})[0][0]
                                          .as<std::string>();
            EXPECT_EQ(ymd(start.value()), vendor_first) << root << ": the series starts where the vendor's does";
            EXPECT_EQ(vendor_first, first) << root;
        }
    }
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars());
    const auto ours = load({"MES.v.0"}, "2026-02-18", "2026-02-26");
    std::map<std::string, const Bar*> on;
    for (const auto& b : ours) on[core::format_utc_date(b.timestamp)] = &b;
    ASSERT_EQ(on.size(), 8u);
    for (const char* day : {"2026-02-18", "2026-02-19", "2026-02-20", "2026-02-22"}) {
        EXPECT_EQ(on.at(day)->instrument_id, "42140878") << day << ": ESH6";
        EXPECT_FALSE(on.at(day)->roll_from.has_value()) << day;
    }
    EXPECT_DOUBLE_EQ(static_cast<double>(on.at("2026-02-22")->close), 6906.25) << "ESH6's own Sunday bar";
    const Bar& change = *on.at("2026-02-23");
    EXPECT_EQ(change.instrument_id, "42003800") << "MESH6";
    EXPECT_DOUBLE_EQ(static_cast<double>(change.close), 6857.25);
    ASSERT_TRUE(change.roll_from.has_value());
    EXPECT_EQ(change.roll_from->kind, RollFromKind::SOURCE_CHANGE) << "HD 2026-10-26: no trade";
    EXPECT_EQ(change.roll_from->instrument_id, "42140878");
    EXPECT_DOUBLE_EQ(change.roll_from->close, 6857.25);
    EXPECT_EQ(change.roll_from->date, "2026-02-23");
    const Bar& after = *on.at("2026-02-24");
    EXPECT_EQ(after.instrument_id, "42003800");
    ASSERT_TRUE(after.roll_from.has_value());
    EXPECT_EQ(after.roll_from->kind, RollFromKind::AFTER_SOURCE_CHANGE);
    EXPECT_EQ(after.roll_from->instrument_id, "42140878");
    EXPECT_EQ(after.roll_from->date, "2026-02-23");
    // 2019, around the micros' listing: the ES contract the vendor names, its close.
    PerContractSource::instance().disable();
    const auto vendor = load({"MES.v.0"}, "2019-04-29", "2019-05-10");
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars());
    const auto listing = load({"MES.v.0"}, "2019-04-29", "2019-05-10");
    ASSERT_EQ(listing.size(), vendor.size());
    ASSERT_FALSE(listing.empty());
    for (size_t i = 0; i < listing.size(); ++i) {
        EXPECT_EQ(listing[i].close, vendor[i].close) << core::format_utc_date(listing[i].timestamp);
        EXPECT_EQ(listing[i].instrument_id, vendor[i].instrument_id);
    }
}

// Carry (trade-ngin #185): every bar carries the quote of the contract the book holds on it, the pair
// moving on the bar that confirms a roll. GC's cycle is G J M Q Z: U is never a neighbour.
TEST(PerContractSourceCarry, ThePairMovesOnTheBarThatConfirmsTheRoll) {
    for (SwitchBar setting : {SwitchBar::HELD, SwitchBar::VENDOR}) {
        SCOPED_TRACE(to_string(setting));
        auto bars = shapes_world().carry_bars_until("2026-08-05", setting);
        ASSERT_EQ(bars.size(), kDates.size());
        for (const auto& [day, b] : bars) {
            const std::string dt = ymd(day);
            SCOPED_TRACE(dt);
            ASSERT_TRUE(b.carry.has_value());
            if (dt <= "2026-07-20") {
                // Q held and the front: the further cycle contract, December. On the confirmed
                // switch bar 07-20 the old contract is still held (the next bar confirms), and
                // the stray of 07-16 changes nothing; whichever bar the setting hands out.
                EXPECT_EQ(b.carry->held_symbol, "GCQ6");
                EXPECT_EQ(b.carry->held_close, close_of("GCQ6", dt));
                EXPECT_EQ(b.carry->other_symbol, "GCZ6");
                EXPECT_EQ(b.carry->other_close, close_of("GCZ6", dt));
                EXPECT_EQ(b.carry->months, 4);
            } else {
                // From the confirming bar 07-21 Z is held and August, still alive, is nearer;
                // the unconfirmed switch of 08-05 to G7 keeps Z.
                EXPECT_EQ(b.carry->held_symbol, "GCZ6");
                EXPECT_EQ(b.carry->held_close, close_of("GCZ6", dt));
                EXPECT_EQ(b.carry->other_symbol, "GCQ6");
                EXPECT_EQ(b.carry->other_close, close_of("GCQ6", dt));
                EXPECT_EQ(b.carry->months, -4);
            }
        }
    }
}

TEST(PerContractSourceCarry, ARollKnownOnItsBarMovesThePairOnThatBar) {
    auto bars = crossover_world().carry_bars_until("2026-08-05");
    ASSERT_TRUE(bars.at(d("2026-07-24")).carry.has_value());
    EXPECT_EQ(bars.at(d("2026-07-24")).carry->held_symbol, "GCQ6");
    EXPECT_EQ(bars.at(d("2026-07-24")).carry->months, 4);
    const SourceBar& roll = bars.at(d("2026-07-27"));
    ASSERT_TRUE(roll.carry.has_value());
    EXPECT_EQ(roll.carry->held_symbol, "GCZ6");
    EXPECT_EQ(roll.carry->held_close, close_of("GCZ6", "2026-07-27"));
    EXPECT_EQ(roll.carry->other_symbol, "GCQ6");
    EXPECT_EQ(roll.carry->months, -4);
}

TEST(PerContractSourceCarry, TheQuotesChangeNoOtherFieldOfABar) {
    auto plain = shapes_world().bars_until("2026-08-05");
    auto carry = shapes_world().carry_bars_until("2026-08-05");
    ASSERT_EQ(plain.size(), carry.size());
    for (const auto& [day, b] : plain) {
        EXPECT_TRUE(same(b, carry.at(day))) << ymd(day);
        EXPECT_FALSE(b.carry.has_value()) << ymd(day);
    }
}

TEST(PerContractSourceCarry, AHeldContractWithNoRowOfItsOwnHasNoQuote) {
    // Q has no row on 07-22: the source carries its close (bar source "carry"); no quote that day.
    World w;
    for (const auto& dt : kDates) {
        if (dt != "2026-07-22") w.print("GCQ6", dt, 80000, close_of("GCQ6", dt));
        w.print("GCZ6", dt, 9000, close_of("GCZ6", dt));
        w.vendor(dt, "GCQ6");
    }
    auto bars = w.carry_bars_until("2026-07-23");
    EXPECT_FALSE(bars.at(d("2026-07-22")).carry.has_value());
    EXPECT_TRUE(bars.at(d("2026-07-21")).carry.has_value());
    EXPECT_TRUE(bars.at(d("2026-07-23")).carry.has_value());
}

TEST(PerContractSourceCarry, TheArrowTableCarriesTheQuoteOnlyWithCarryOn) {
    auto carry = shapes_world().carry_bars_until("2026-08-05");
    std::vector<SourceBar> rows;
    for (const auto& [day, b] : carry) rows.push_back(b);
    rows.back().carry.reset();  // a day with no quote
    // tune2/final-base (the merge of MM's fix round onto carry): MM's two columns ride along in
    // every table, so give some rows both fields
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i % 2 == 0) rows[i].own_contract_volume = 1000.0 + static_cast<double>(i);
        if (rows[i].roll_from && i % 3 == 0) rows[i].roll_from->volume = 5000.0 + static_cast<double>(i);
    }
    // Carry off: the fourteen columns as before (MM's roll_from_volume and own_contract_volume the
    // thirteenth and fourteenth), and no bar carries a quote.
    auto off = source_bars_to_arrow(rows);
    ASSERT_TRUE(off.is_ok());
    EXPECT_EQ(off.value()->num_columns(), 14);
    auto off_bars = DataConversionUtils::arrow_table_to_bars(off.value());
    ASSERT_TRUE(off_bars.is_ok());
    for (const auto& b : off_bars.value()) EXPECT_FALSE(b.carry.has_value());
    // Carry on: five more columns, the quote round-trips exactly, NULL stays absent.
    auto on = source_bars_to_arrow(rows, true);
    ASSERT_TRUE(on.is_ok());
    EXPECT_EQ(on.value()->num_columns(), 19);
    EXPECT_NE(on.value()->GetColumnByName("carry_months"), nullptr);
    // the merged layout, column by column: MM's two at 12 and 13 in both tables, carry's five after
    const std::vector<std::string> layout = {"time", "symbol", "open", "high", "low", "close", "volume",
                                             "instrument_id", "roll_from_close", "roll_from_id",
                                             "roll_from_kind", "roll_from_date", "roll_from_volume",
                                             "own_contract_volume", "carry_held_close", "carry_other_close",
                                             "carry_months", "carry_held_symbol", "carry_other_symbol"};
    for (int c = 0; c < on.value()->num_columns(); ++c) {
        EXPECT_EQ(on.value()->schema()->field(c)->name(), layout[static_cast<size_t>(c)]) << c;
        if (c < off.value()->num_columns()) {
            EXPECT_EQ(off.value()->schema()->field(c)->name(), layout[static_cast<size_t>(c)]) << c;
        }
    }
    auto on_bars = DataConversionUtils::arrow_table_to_bars(on.value());
    ASSERT_TRUE(on_bars.is_ok()) << on_bars.error()->what();
    ASSERT_EQ(on_bars.value().size(), rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(on_bars.value()[i].carry, rows[i].carry) << i;
        EXPECT_EQ(on_bars.value()[i].roll_from, off_bars.value()[i].roll_from) << i;
        EXPECT_EQ(on_bars.value()[i].close, off_bars.value()[i].close) << i;
        // every field read back by name, carry on and off: MM's two as written
        EXPECT_EQ(on_bars.value()[i].roll_from, rows[i].roll_from) << i;
        EXPECT_EQ(on_bars.value()[i].own_contract_volume, rows[i].own_contract_volume) << i;
        EXPECT_EQ(off_bars.value()[i].own_contract_volume, rows[i].own_contract_volume) << i;
        EXPECT_EQ(on_bars.value()[i].instrument_id, rows[i].instrument_id) << i;
        EXPECT_EQ(on_bars.value()[i].volume, rows[i].volume) << i;
    }
    EXPECT_FALSE(on_bars.value().back().carry.has_value());
    // A table with only part of the quote's columns is refused, never read as no quote.
    auto partial = on.value()->RemoveColumn(on.value()->schema()->GetFieldIndex("carry_months"));
    ASSERT_TRUE(partial.ok());
    EXPECT_TRUE(DataConversionUtils::arrow_table_to_bars(*partial).is_error());
}

TEST(PerContractSourceCarry, CarryNeedsThePerContractSourceAndTheSwitchSetsTheQuotes) {
    auto refused = configure_per_contract_source(false, "held", true);
    ASSERT_TRUE(refused.is_error());
    EXPECT_NE(std::string(refused.error()->what()).find("per_contract_rolls is off"),
              std::string::npos);
    EXPECT_FALSE(PerContractSource::instance().enabled());
    EXPECT_FALSE(PerContractSource::instance().carry_quotes());
    if (contract_rolls_test::shipped_futures_calendar_path().empty()) GTEST_SKIP();
    const char* before = std::getenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON");
    const std::string saved = before ? before : "";
    setenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON",
           contract_rolls_test::shipped_futures_calendar_path().c_str(), 1);
    ASSERT_TRUE(configure_per_contract_source(true, "held", true).is_ok());
    EXPECT_TRUE(PerContractSource::instance().carry_quotes());
    ASSERT_TRUE(configure_per_contract_source(true, "held").is_ok());
    EXPECT_FALSE(PerContractSource::instance().carry_quotes());
    PerContractSource::instance().disable();
    if (saved.empty()) {
        unsetenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON");
    } else {
        setenv("TRADE_NGIN_FUTURES_HOLIDAYS_JSON", saved.c_str(), 1);
    }
}

// Carry (fix round 1, audit A S1): the one production path that puts quotes on bars, the loader's own
// call (get_market_data -> per_contract_bars -> attach_carry_quotes -> the five Arrow columns ->
// Bar::carry), on the clone. Each quote's two closes are the contract table's own rows of the bar's date,
// the pair moves on GC's crossover bar of 2026-07-29, and with the quotes off the table is today's.
TEST_F(PerContractSourceDbTest, TheLoadersOwnCallHandsOutCarryQuotesOnlyWithCarryOn) {
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars(), SwitchBar::HELD, true);
    const auto quoted = load({"GC.v.0", "ZN.v.0"}, "2026-07-01", "2026-08-31");
    ASSERT_FALSE(quoted.empty());
    pqxx::connection c(contract_rolls_test::dsn());
    pqxx::read_transaction t(c);
    auto close_of = [&](const std::string& raw, const std::string& date) {
        auto r = t.exec("SELECT close FROM futures_data.contract_ohlcv_1d WHERE raw_symbol = $1 AND date = $2 "
                        "AND expiration > $2::date - 400",
                        pqxx::params{raw, date});
        EXPECT_EQ(r.size(), 1u) << raw << " " << date;
        return r.empty() ? -1.0 : r[0][0].as<double>();
    };
    std::map<std::string, int> with_quote, bars;
    for (const auto& b : quoted) {
        ++bars[b.symbol];
        if (!b.carry) continue;
        ++with_quote[b.symbol];
        const std::string date = core::format_utc_date(b.timestamp);
        EXPECT_NE(b.carry->months, 0) << b.symbol << " " << date;
        EXPECT_EQ(b.carry->held_close, close_of(b.carry->held_symbol, date)) << b.symbol << " " << date;
        EXPECT_EQ(b.carry->other_close, close_of(b.carry->other_symbol, date)) << b.symbol << " " << date;
        if (b.symbol == "GC.v.0") {
            // GC's cycle G J M Q Z: before the 07-29 crossover Q6 is held, the front, against Z6;
            // from it Z6 is held against the nearer Q6 while Q6 is alive (it expires 08-27); after
            // that Z6 is the front, against the further G7
            const std::string held = date < "2026-07-29" ? "GCQ6" : "GCZ6";
            const std::string other = date < "2026-07-29" ? "GCZ6" : date <= "2026-08-27" ? "GCQ6" : "GCG7";
            const int months = date < "2026-07-29" ? 4 : date <= "2026-08-27" ? -4 : 2;
            EXPECT_EQ(b.carry->held_symbol, held) << date;
            EXPECT_EQ(b.carry->other_symbol, other) << date;
            EXPECT_EQ(b.carry->months, months) << date;
        }
    }
    for (const char* s : {"GC.v.0", "ZN.v.0"}) {
        ASSERT_GT(bars[s], 30) << s;
        EXPECT_GT(with_quote[s], bars[s] * 3 / 4) << s << ": most bars carry a quote";
    }
    // Carry off: the same call hands out no quote and the table is the twelve columns of today.
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars(), SwitchBar::HELD, false);
    const auto plain = load({"GC.v.0", "ZN.v.0"}, "2026-07-01", "2026-08-31");
    ASSERT_EQ(plain.size(), quoted.size());
    for (size_t i = 0; i < plain.size(); ++i) {
        EXPECT_FALSE(plain[i].carry.has_value());
        EXPECT_EQ(plain[i].close, quoted[i].close);
        EXPECT_EQ(plain[i].instrument_id, quoted[i].instrument_id);
        EXPECT_EQ(plain[i].roll_from, quoted[i].roll_from);
    }
    const auto ts = [](const std::string& s) {
        return std::chrono::system_clock::time_point(std::chrono::sys_days(d(s)));
    };
    auto table = db_->get_market_data({"GC.v.0"}, ts("2026-07-01"), ts("2026-08-31"), AssetClass::FUTURES);
    ASSERT_TRUE(table.is_ok());
    EXPECT_EQ(table.value()->num_columns(), 14);
    PerContractSource::instance().enable(contract_rolls_test::shipped_calendars(), SwitchBar::HELD, true);
    auto table_on = db_->get_market_data({"GC.v.0"}, ts("2026-07-01"), ts("2026-08-31"), AssetClass::FUTURES);
    ASSERT_TRUE(table_on.is_ok());
    EXPECT_EQ(table_on.value()->num_columns(), 19);
}
