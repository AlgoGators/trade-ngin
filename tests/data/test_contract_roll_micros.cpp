// The micros' alias (ROLLS_STEP1_PLAN 10b point 5): before the vendor's 2026-02-22 change, MES.v.0 is
// the ES contract, so the series reads ES contracts (and rolls among them by the rule); from the change,
// MES's own. The change is one roll between two contracts of the same expiry, on the first trading-day
// bar on or after 02-22 (a Sunday) where both print. Fixtures here; the
// database tests (ContractRollsApiDbTest, PerContractSourceDbTest) read the loaded ES contracts.
#include <gtest/gtest.h>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/contract_roll_rule.hpp"

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
