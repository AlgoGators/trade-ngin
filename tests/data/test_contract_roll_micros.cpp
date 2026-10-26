// The micros' alias (ROLLS_STEP1_PLAN 10b point 5): before the vendor's 2026-02-22 change, MES.v.0 is
// the ES contract, so the series reads ES contracts (and rolls among them by the rule); from the change,
// MES's own. The change is one roll between two contracts of the same expiry, on the first trading-day
// bar on or after 02-22 (a Sunday) where both print. Fixtures here; the
// database tests (ContractRollsApiDbTest, PerContractSourceDbTest) read the loaded ES contracts.
#include <gtest/gtest.h>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/contract_roll_rule.hpp"
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
