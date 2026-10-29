// The micros' alias (ROLLS_STEP1_PLAN 10b point 5): before the vendor's 2026-02-22 change, MES.v.0 is
// the ES contract, so the series reads ES contracts (and rolls among them by the rule); from the change,
// MES's own. The change is one roll between two contracts of the same expiry, on the first trading-day
// bar on or after 02-22 (a Sunday) where both print. Fixtures here; the
// database tests (ContractRollsApiDbTest, PerContractSourceDbTest) read the loaded ES contracts.
#include <gtest/gtest.h>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/contract_roll_rule.hpp"
#include "trade_ngin/data/contract_rolls.hpp"
#include "trade_ngin/data/per_contract_source.hpp"
#include "trade_ngin/data/roll_series.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::bar;
using contract_rolls_test::d;

namespace {

// Weekdays and Sundays from 2025-12-08 to 2026-03-06.
std::vector<std::string> grid_dates() {
    std::vector<std::string> out;
    for (Day x = d("2025-12-08"); x <= d("2026-03-06"); x += std::chrono::days{1}) {
        if (std::chrono::weekday{x} != std::chrono::Saturday) out.push_back(ymd(x));
    }
    return out;
}

}  // namespace

TEST(ContractRollMicros, AliasTableNamesTheFourMicros) {
    ASSERT_TRUE(micro_alias("MES").has_value());
    EXPECT_EQ(micro_alias("MES")->full_root, "ES");
    EXPECT_EQ(micro_alias("MNQ")->full_root, "NQ");
    EXPECT_EQ(micro_alias("MYM")->full_root, "YM");
    EXPECT_EQ(micro_alias("M2K")->full_root, "RTY");
    EXPECT_EQ(ymd(micro_alias("M2K")->change), "2026-02-22");
    EXPECT_FALSE(micro_alias("ES").has_value());
    EXPECT_FALSE(micro_alias("GC").has_value());
}

TEST(ContractRollMicros, ReadsTheFullSizeContractsBeforeTheChangeAndRollsOnceAcrossIt) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (const auto& dt : grid_dates()) {
        const bool sunday = std::chrono::weekday{d(dt)} == std::chrono::Sunday;
        // ES: Dec 2025 (expires 12-19), Mar 2026 (03-20); no Sunday bars (the 02-22 Sunday needs the
        // Monday). MES: Mar 2026, its own contract, every grid date.
        if (dt <= "2025-12-19" && !sunday) rows.push_back(bar("ES", "ESZ5", "e0", dt, "2025-12-19", 6800, 1500000));
        if (!sunday) rows.push_back(bar("ES", "ESH6", "e1", dt, "2026-03-20", 6900, dt <= "2025-12-12" ? 9000 : 1600000));
        rows.push_back(bar("MES", "MESH6", "m1", dt, "2026-03-20", 6901, 1700000));
        VendorBar v;
        v.date = d(dt);
        v.symbol = "MES.v.0";
        v.instrument_id = dt >= "2026-02-22" ? "m1" : (dt <= "2025-12-12" ? "e0" : "e1");
        v.close = 1;
        grid.push_back(v);
    }
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok()) << book.error()->what();
    const TradingCalendar cal = contract_rolls_test::shipped_calendar("MES");
    RuleInputs in;
    in.root = "MES";
    in.grid = grid;
    in.book = &book.value();
    in.cal = &cal;
    in.cycle = roll_cycle("MES").value();
    in.alias = micro_alias("MES");
    auto r = apply_roll_rule(in);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    std::vector<const HeldDay*> rolls;
    for (const auto& day : r.value().days) {
        if (day.roll) rolls.push_back(&day);
    }
    ASSERT_EQ(rolls.size(), 2u);
    // ESZ5 -> ESH6 among the full-size contracts, by the vendor's switch of Monday 12-15.
    EXPECT_EQ(ymd(rolls[0]->date), "2025-12-15");
    EXPECT_EQ(rolls[0]->old->root, "ES");
    EXPECT_EQ(rolls[0]->held.raw_symbol, "ESH6");
    EXPECT_EQ(rolls[0]->trigger, RollTrigger::VENDOR);
    // The change: ESH6 -> MESH6, same expiry, on Monday 02-23 (02-22 is a Sunday).
    EXPECT_EQ(ymd(rolls[1]->date), "2026-02-23");
    EXPECT_EQ(rolls[1]->trigger, RollTrigger::ALIAS);
    EXPECT_EQ(rolls[1]->old->raw_symbol, "ESH6");
    EXPECT_EQ(rolls[1]->held.raw_symbol, "MESH6");
    EXPECT_EQ(rolls[1]->held.expiration, rolls[1]->old->expiration);
    EXPECT_EQ(*rolls[1]->old_close, 6900);
    EXPECT_EQ(rolls[1]->close, 6901);
    for (const auto& day : r.value().days) {
        if (day.date == d("2026-02-20")) {
            EXPECT_EQ(day.held.root, "ES");
            EXPECT_EQ(day.bar_id, "e1");
            EXPECT_EQ(day.vendor->raw_symbol, "ESH6");
        }
        if (day.date == d("2026-02-22")) {  // Sunday: still ESH6, carried (the vendor names MESH6)
            EXPECT_EQ(day.held.raw_symbol, "ESH6");
            EXPECT_EQ(day.source, BarSource::CARRY);
        }
        if (day.date >= d("2026-02-23")) EXPECT_EQ(day.held.root, "MES") << ymd(day.date);
    }
}

// MM (HD 2026-10-27): the gold and silver micros read their full-size root's contracts before their
// listing dates, MGC GC's before 2010-10-04 and SIL SI's before 2013-06-17, on the full-size root's
// grid (their own .v.0 starts at the listing). The equity micros keep their own grid.
TEST(ContractRollMicros, TheGoldAndSilverMicrosReadTheFullSizeRootUntilTheirListing) {
    ASSERT_TRUE(micro_alias("MGC").has_value());
    EXPECT_EQ(micro_alias("MGC")->full_root, "GC");
    EXPECT_EQ(ymd(micro_alias("MGC")->change), "2010-10-04");
    EXPECT_TRUE(micro_alias("MGC")->full_size_grid);
    ASSERT_TRUE(micro_alias("SIL").has_value());
    EXPECT_EQ(micro_alias("SIL")->full_root, "SI");
    EXPECT_EQ(ymd(micro_alias("SIL")->change), "2013-06-17");
    EXPECT_TRUE(micro_alias("SIL")->full_size_grid);
    for (const char* r : {"MES", "MNQ", "MYM", "M2K"}) EXPECT_FALSE(micro_alias(r)->full_size_grid) << r;
    EXPECT_FALSE(micro_alias("SI").has_value());

    // The grid's rows: one symbol for any other root, the two sides of the change for MGC and SIL.
    const GridRows own = grid_rows("MES", 3);
    EXPECT_EQ(own.predicate, "o.symbol = $3");
    EXPECT_EQ(own.params.size(), 1);
    const GridRows gold = grid_rows("MGC", 4);
    EXPECT_EQ(gold.predicate,
              "((o.symbol = $4 AND (o.time AT TIME ZONE 'UTC')::date >= $5::date) OR (o.symbol = $6 AND "
              "(o.time AT TIME ZONE 'UTC')::date < $5::date))");
    EXPECT_EQ(gold.params.size(), 3);
}

// The three reads of a root's grid, without a database: any root but MGC and SIL reads its own .v.0
// over the range with the same SQL as before; MGC and SIL read the full-size .v.0 to the day before
// their change and their own from it, each side left out when the range does not reach it.
TEST(ContractRollMicros, TheGridReadsSplitAtTheChangeForTheGoldAndSilverMicrosOnly) {
    const auto mes = root_grid_reads("MES", d("2010-06-07"), d("2026-10-23"));
    ASSERT_EQ(mes.size(), 1u);
    EXPECT_EQ(mes[0].symbol, "MES.v.0");
    EXPECT_EQ(ymd(mes[0].from), "2010-06-07");
    EXPECT_EQ(ymd(mes[0].to), "2026-10-23");
    const auto gold = root_grid_reads("MGC", d("2010-06-07"), d("2026-10-23"));
    ASSERT_EQ(gold.size(), 2u);
    EXPECT_EQ(gold[0].symbol, "GC.v.0");
    EXPECT_EQ(ymd(gold[0].from), "2010-06-07");
    EXPECT_EQ(ymd(gold[0].to), "2010-10-03");
    EXPECT_EQ(gold[1].symbol, "MGC.v.0");
    EXPECT_EQ(ymd(gold[1].from), "2010-10-04");
    EXPECT_EQ(ymd(gold[1].to), "2026-10-23");
    const auto before = root_grid_reads("SIL", d("2012-01-02"), d("2013-06-14"));
    ASSERT_EQ(before.size(), 1u);
    EXPECT_EQ(before[0].symbol, "SI.v.0");
    EXPECT_EQ(ymd(before[0].to), "2013-06-14");
    const auto after = root_grid_reads("SIL", d("2013-06-17"), d("2014-01-02"));
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].symbol, "SIL.v.0");

    // series_start's query: the predicate after the three CASE parameters
    const auto [own_sql, own_params] = series_start_query("GC");
    EXPECT_NE(own_sql.find(" WHERE o.symbol = $4 AND EXISTS"), std::string::npos) << own_sql;
    EXPECT_EQ(own_params.size(), 4);
    const auto [gold_sql, gold_params] = series_start_query("MGC");
    EXPECT_NE(gold_sql.find(" WHERE ((o.symbol = $4 AND (o.time AT TIME ZONE 'UTC')::date >= $5::date) OR "
                            "(o.symbol = $6 AND (o.time AT TIME ZONE 'UTC')::date < $5::date)) AND EXISTS"),
              std::string::npos)
        << gold_sql;
    EXPECT_EQ(gold_params.size(), 6);

    // the source's window query: unchanged text for any other root
    const auto [zn_sql, zn_params] = grid_window_query("ZN", "2026-01-01 00:00:00", "2026-02-01 00:00:00");
    EXPECT_EQ(zn_sql,
              "SELECT t.time::text, ((t.time AT TIME ZONE 'UTC')::date)::text FROM (SELECT DISTINCT o.time "
              "FROM futures_data.ohlcv_1d o WHERE o.symbol = $3 AND o.time BETWEEN $1 AND $2) AS t ORDER BY t.time");
    EXPECT_EQ(zn_params.size(), 3);
    const auto [sil_sql, sil_params] = grid_window_query("SIL", "2013-01-01 00:00:00", "2014-01-01 00:00:00");
    EXPECT_NE(sil_sql.find("WHERE ((o.symbol = $3 AND"), std::string::npos) << sil_sql;
    EXPECT_NE(sil_sql.find("(o.symbol = $5 AND"), std::string::npos) << sil_sql;
    EXPECT_EQ(sil_params.size(), 5);
}

// MGC across its listing: GCZ0 is held on GC's grid to Friday 2010-10-01, and on Monday 10-04 the
// change rolls once into MGCZ0 (same expiry 2010-12-29), trigger alias, read like R6's change.
TEST(ContractRollMicros, MgcReadsGcZ0UntilItsListingAndChangesOnceIntoMgcZ0) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (Day x = d("2010-09-13"); x <= d("2010-10-22"); x += std::chrono::days{1}) {
        const std::string dt = ymd(x);
        const bool sunday = std::chrono::weekday{x} == std::chrono::Sunday;
        if (std::chrono::weekday{x} == std::chrono::Saturday) continue;
        if (!sunday) rows.push_back(bar("GC", "GCZ0", "g1", dt, "2010-12-29", 1300, 150000));
        if (dt >= "2010-10-03") rows.push_back(bar("MGC", "MGCZ0", "m1", dt, "2010-12-29", 1300.5, 60));
        VendorBar v;
        v.date = x;
        v.symbol = dt < "2010-10-04" ? "GC.v.0" : "MGC.v.0";
        v.instrument_id = dt < "2010-10-04" ? "g1" : "m1";
        v.close = 1;
        grid.push_back(v);
    }
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok()) << book.error()->what();
    const TradingCalendar cal = contract_rolls_test::shipped_calendar("MGC");
    RuleInputs in;
    in.root = "MGC";
    in.grid = grid;
    in.book = &book.value();
    in.cal = &cal;
    in.cycle = roll_cycle("MGC").value();
    in.alias = micro_alias("MGC");
    auto r = apply_roll_rule(in);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    std::vector<const HeldDay*> rolls;
    for (const auto& day : r.value().days) {
        if (day.roll) rolls.push_back(&day);
        if (day.date < d("2010-10-04")) {
            EXPECT_EQ(day.held.raw_symbol, "GCZ0") << ymd(day.date);
        } else {
            EXPECT_EQ(day.held.raw_symbol, "MGCZ0") << ymd(day.date);
        }
    }
    ASSERT_EQ(rolls.size(), 1u);
    EXPECT_EQ(ymd(rolls[0]->date), "2010-10-04");
    EXPECT_EQ(rolls[0]->trigger, RollTrigger::ALIAS);
    EXPECT_FALSE(rolls[0]->deferred);
    EXPECT_EQ(rolls[0]->old->raw_symbol, "GCZ0");
    EXPECT_EQ(*rolls[0]->old_close, 1300);
    EXPECT_EQ(rolls[0]->close, 1300.5);
}

// SIL across its listing (MM step 0): on Monday 2013-06-17 SI holds SIN3 (July), and SIL lists no July:
// its first contracts are September onwards. The change goes into SIL's cycle target, SILU3 (the
// contract SIL.v.0 names from its first bar), once, trigger alias; SI's own July-to-September roll
// never happens in SIL's series.
TEST(ContractRollMicros, SilWithNoContractOfTheHeldExpiryChangesIntoItsCycleTarget) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    for (Day x = d("2013-06-03"); x <= d("2013-07-12"); x += std::chrono::days{1}) {
        const std::string dt = ymd(x);
        const bool sunday = std::chrono::weekday{x} == std::chrono::Sunday;
        if (std::chrono::weekday{x} == std::chrono::Saturday) continue;
        if (!sunday) {
            rows.push_back(bar("SI", "SIN3", "s7", dt, "2013-07-29", 21.83, 30000));
            rows.push_back(bar("SI", "SIU3", "s9", dt, "2013-09-26", 21.85, 800));
        }
        if (dt >= "2013-06-17") rows.push_back(bar("SIL", "SILU3", "l9", dt, "2013-09-26", 21.80, 20));
        VendorBar v;
        v.date = x;
        v.symbol = dt < "2013-06-17" ? "SI.v.0" : "SIL.v.0";
        v.instrument_id = dt < "2013-06-17" ? "s7" : "l9";
        v.close = 1;
        grid.push_back(v);
    }
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok()) << book.error()->what();
    const TradingCalendar cal = contract_rolls_test::shipped_calendar("SIL");
    RuleInputs in;
    in.root = "SIL";
    in.grid = grid;
    in.book = &book.value();
    in.cal = &cal;
    in.cycle = roll_cycle("SIL").value();
    in.alias = micro_alias("SIL");
    auto r = apply_roll_rule(in);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    std::vector<const HeldDay*> rolls;
    for (const auto& day : r.value().days) {
        if (day.roll) rolls.push_back(&day);
        EXPECT_EQ(day.held.raw_symbol, day.date < d("2013-06-17") ? "SIN3" : "SILU3") << ymd(day.date);
    }
    ASSERT_EQ(rolls.size(), 1u);
    EXPECT_EQ(ymd(rolls[0]->date), "2013-06-17");
    EXPECT_EQ(rolls[0]->trigger, RollTrigger::ALIAS);
    EXPECT_EQ(rolls[0]->old->raw_symbol, "SIN3");
    EXPECT_EQ(*rolls[0]->old_close, 21.83);
    EXPECT_EQ(rolls[0]->close, 21.80);
}

// R6 (HD 2026-10-26): the bar source hands the change bar out as a change of price source
// (SOURCE_CHANGE, ESH6's close and id) and every bar after it, while ESH6 prints, as AFTER_SOURCE_CHANGE;
// the earlier roll among the full-size contracts (ESZ5 to ESH6) stays KNOWN and AFTER_ROLL.
TEST(ContractRollMicros, TheChangeIsHandedOutAsAChangeOfPriceSource) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    std::map<Day, std::string> window;
    for (const auto& dt : grid_dates()) {
        const bool sunday = std::chrono::weekday{d(dt)} == std::chrono::Sunday;
        if (dt <= "2025-12-19" && !sunday) rows.push_back(bar("ES", "ESZ5", "e0", dt, "2025-12-19", 6800, 1500000));
        if (!sunday) rows.push_back(bar("ES", "ESH6", "e1", dt, "2026-03-20", 6900, dt <= "2025-12-12" ? 9000 : 1600000));
        rows.push_back(bar("MES", "MESH6", "m1", dt, "2026-03-20", 6901, 1700000));
        VendorBar v;
        v.date = d(dt);
        v.symbol = "MES.v.0";
        v.instrument_id = dt >= "2026-02-22" ? "m1" : (dt <= "2025-12-12" ? "e0" : "e1");
        v.close = 1;
        grid.push_back(v);
        window[d(dt)] = dt + " 00:00:00+00";
    }
    auto book = ContractBook::build(rows).value();
    const TradingCalendar cal = contract_rolls_test::shipped_calendar("MES");
    RuleInputs in;
    in.root = "MES";
    in.grid = grid;
    in.book = &book;
    in.cal = &cal;
    in.cycle = roll_cycle("MES").value();
    in.alias = micro_alias("MES");
    const auto series = apply_roll_rule(in);
    ASSERT_TRUE(series.is_ok());
    const auto bars = bars_of_series(series.value(), window);
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    std::map<std::string, const SourceBar*> on;
    for (const auto& b : bars.value()) on[ymd(b.date)] = &b;
    ASSERT_TRUE(on.at("2026-02-23")->roll_from.has_value());
    EXPECT_EQ(on.at("2026-02-23")->roll_from->kind, trade_ngin::RollFromKind::SOURCE_CHANGE);
    EXPECT_EQ(on.at("2026-02-23")->roll_from->instrument_id, "e1");
    EXPECT_EQ(on.at("2026-02-24")->roll_from->kind, trade_ngin::RollFromKind::AFTER_SOURCE_CHANGE);
    EXPECT_EQ(on.at("2026-02-24")->roll_from->date, "2026-02-23");
    EXPECT_EQ(on.at("2025-12-15")->roll_from->kind, trade_ngin::RollFromKind::KNOWN) << "a real roll";
    EXPECT_EQ(on.at("2025-12-16")->roll_from->kind, trade_ngin::RollFromKind::AFTER_ROLL);
    // MM fix round 1: the change's two kinds carry ESH6's own volume (a listing predecessor's legs
    // are priced on it); the bar itself is MESH6's, with MESH6's volume; a real roll's kinds carry none.
    EXPECT_EQ(on.at("2026-02-23")->volume, 1700000.0);
    EXPECT_EQ(on.at("2026-02-23")->roll_from->volume, std::optional<double>(1600000.0));
    EXPECT_EQ(on.at("2026-02-24")->roll_from->volume, std::optional<double>(1600000.0));
    EXPECT_FALSE(on.at("2025-12-15")->roll_from->volume.has_value());
    EXPECT_FALSE(on.at("2025-12-16")->roll_from->volume.has_value());
}

// R6 fix round 1 (audit A S1): the first REAL roll after the change is an ordinary roll. MES runs past the
// change of price source (Monday 02-23, ESH6 to MESH6) and on through its March roll: MESM6 out-trades MESH6
// from Monday 03-16, inside MESH6's window (from Fri 03-13, the fifth CME trading day before its last
// trading day Fri 03-20), so the rule rolls there on the crossover. That roll is handed out KNOWN and the
// bars after it AFTER_ROLL (never the change's kinds); a consumer that reads every bar confirms it on 03-16
// as a roll (no price-source flag, its two legs and its split), and a consumer that withheld the 03-16 bar
// books it on 03-17 as a roll too.
TEST(ContractRollMicros, TheFirstRealRollAfterTheChangeIsAnOrdinaryRoll) {
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    std::map<Day, std::string> window;
    for (Day x = d("2026-02-02"); x <= d("2026-03-27"); x += std::chrono::days{1}) {
        const auto wd = std::chrono::weekday{x};
        if (wd == std::chrono::Saturday) continue;
        const bool sunday = wd == std::chrono::Sunday;
        const std::string dt = ymd(x);
        const double k = static_cast<double>((x - d("2026-02-02")).count());
        if (!sunday && dt <= "2026-03-20") rows.push_back(bar("ES", "ESH6", "e1", dt, "2026-03-20", 6900 + k, 1600000));
        if (!sunday) rows.push_back(bar("ES", "ESM6", "e2", dt, "2026-06-18", 6950 + k, 9000));
        if (dt <= "2026-03-20") rows.push_back(bar("MES", "MESH6", "m1", dt, "2026-03-20", 6901 + k, 1700000));
        rows.push_back(bar("MES", "MESM6", "m2", dt, "2026-06-18", 6951 + k, dt >= "2026-03-16" ? 2000000 : 100));
        VendorBar v;
        v.date = x;
        v.symbol = "MES.v.0";
        v.instrument_id = dt >= "2026-03-18" ? "m2" : (dt >= "2026-02-22" ? "m1" : "e1");
        v.close = 1;
        grid.push_back(v);
        window[x] = dt + " 00:00:00+00";
    }
    auto book = ContractBook::build(rows).value();
    const TradingCalendar cal = contract_rolls_test::shipped_calendar("MES");
    RuleInputs in;
    in.root = "MES";
    in.grid = grid;
    in.book = &book;
    in.cal = &cal;
    in.cycle = roll_cycle("MES").value();
    in.alias = micro_alias("MES");
    const auto series = apply_roll_rule(in);
    ASSERT_TRUE(series.is_ok()) << series.error()->what();
    std::vector<const HeldDay*> rolls;
    for (const auto& day : series.value().days) {
        if (day.roll) rolls.push_back(&day);
    }
    ASSERT_EQ(rolls.size(), 2u);
    EXPECT_EQ(rolls[0]->trigger, RollTrigger::ALIAS);
    EXPECT_EQ(ymd(rolls[0]->date), "2026-02-23");
    EXPECT_EQ(rolls[1]->trigger, RollTrigger::CROSSOVER);
    EXPECT_EQ(ymd(rolls[1]->date), "2026-03-16");
    EXPECT_EQ(rolls[1]->old->raw_symbol, "MESH6");
    EXPECT_EQ(rolls[1]->held.raw_symbol, "MESM6");

    const auto bars = bars_of_series(series.value(), window);
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    std::map<std::string, const SourceBar*> on;
    for (const auto& b : bars.value()) on[ymd(b.date)] = &b;
    using trade_ngin::RollFromKind;
    EXPECT_EQ(on.at("2026-02-23")->roll_from->kind, RollFromKind::SOURCE_CHANGE);
    EXPECT_EQ(on.at("2026-03-13")->roll_from->kind, RollFromKind::AFTER_SOURCE_CHANGE) << "ESH6 still prints";
    ASSERT_TRUE(on.at("2026-03-16")->roll_from.has_value());
    EXPECT_EQ(on.at("2026-03-16")->roll_from->kind, RollFromKind::KNOWN) << "the real roll";
    EXPECT_EQ(on.at("2026-03-16")->roll_from->instrument_id, "m1");
    for (const char* after : {"2026-03-17", "2026-03-18", "2026-03-19", "2026-03-20"}) {
        ASSERT_TRUE(on.at(after)->roll_from.has_value()) << after;
        EXPECT_EQ(on.at(after)->roll_from->kind, RollFromKind::AFTER_ROLL) << after << ": MESH6 left by a real roll";
        EXPECT_EQ(on.at(after)->roll_from->instrument_id, "m1") << after;
    }

    // Two consumers: every bar, and one that withheld the 03-16 roll bar.
    for (const bool withhold : {false, true}) {
        SCOPED_TRACE(withhold ? "the roll bar withheld" : "every bar consumed");
        trade_ngin::roll_series::RollTracker tracker;
        int source_changes = 0, rolls_seen = 0;
        for (const auto& b : bars.value()) {
            if (withhold && ymd(b.date) == "2026-03-16") continue;
            const auto st = tracker.add(b.instrument_id, b.close, b.roll_from);
            if (!st.confirm) continue;
            if (st.price_source_change) {
                ++source_changes;
                EXPECT_EQ(ymd(b.date), "2026-02-23");
                continue;
            }
            ++rolls_seen;
            EXPECT_EQ(ymd(b.date), withhold ? "2026-03-17" : "2026-03-16");
            EXPECT_EQ(st.previous_held_id, "m1");
            EXPECT_EQ(st.held_id, "m2");
            ASSERT_TRUE(st.roll_bar_points.has_value());
            auto legs = trade_ngin::roll_series::make_roll_legs(
                "MES.v.0", 2.0, st.last_close_before_change, st.change_bar_close, st.previous_held_id, st.held_id,
                trade_ngin::Timestamp{}, "RC", "RC", "RO", "RO",
                [](const std::string&, double, double) { return trade_ngin::roll_series::RollLegCost{1, 0, 0, 1}; });
            trade_ngin::roll_series::attach_roll_split(legs, 2.0, *st.roll_bar_points, 5.0);
            ASSERT_EQ(legs.size(), 2u) << "an ordinary roll is legged";
            ASSERT_TRUE(legs[0].roll_gap_points.has_value()) << "and its closing leg carries the split";
            EXPECT_DOUBLE_EQ(*legs[0].roll_gap_points, st.change_bar_close - st.last_close_before_change);
            EXPECT_DOUBLE_EQ(*legs[0].roll_gap_points, 50.0) << "MESM6 is 50 over MESH6 every day";
        }
        EXPECT_EQ(source_changes, 1) << "the change of price source, once";
        EXPECT_EQ(rolls_seen, 1) << "the March roll, a roll";
    }
}
