// The roll cycle: the delivery months the vendor's series has held in at
// least half of the years it covers; "held" is named on two consecutive grid bars, so a one-bar
// stray never counts. The fixed table answers the target and the email's warning.
#include <gtest/gtest.h>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/roll_cycle.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::bar;
using contract_rolls_test::d;

namespace {

VendorBar vbar(const std::string& date, const std::string& id) {
    VendorBar v;
    v.date = d(date);
    v.symbol = "XX.v.0";
    v.instrument_id = id;
    v.close = 1.0;
    v.volume = 1;
    return v;
}

}  // namespace

TEST(RollCycle, TableLookupsAndTheNextCycleMonth) {
    EXPECT_EQ(roll_cycle("GC").value(), "GJMQZ");
    EXPECT_EQ(roll_cycle("ZC").value(), "HKNZ");
    EXPECT_EQ(roll_cycle("6A").value(), "HMUZ");
    // The gold and silver micros hold their full-size roots' months (measured on their own .v.0)
    EXPECT_EQ(roll_cycle("MGC").value(), roll_cycle("GC").value());
    EXPECT_EQ(roll_cycle("SIL").value(), roll_cycle("SI").value());
    EXPECT_TRUE(roll_cycle("XX").is_error());
    EXPECT_TRUE(in_cycle("GJMQZ", 8));    // Aug
    EXPECT_FALSE(in_cycle("GJMQZ", 10));  // Oct: never held
    EXPECT_FALSE(in_cycle("GJMQZ", 9));
    // GCQ6 -> GCZ6, GCZ6 -> GCG7 (skipping Sep, Oct, Nov, Jan).
    EXPECT_EQ(next_cycle_delivery("GJMQZ", 2026, 8), 2026 * 12 + 11);
    EXPECT_EQ(next_cycle_delivery("GJMQZ", 2026, 12), 2027 * 12 + 1);
    // ZCN -> ZCZ (Sep is not in the cycle).
    EXPECT_EQ(next_cycle_delivery("HKNZ", 2026, 7), 2026 * 12 + 11);
    EXPECT_EQ(next_cycle_delivery("", 2026, 7), -1);
    // Every root the rule trades has a cycle, and every cycle is month codes in calendar order.
    for (const auto& [root, cycle] : roll_cycle_table()) {
        ASSERT_FALSE(cycle.empty()) << root;
        for (size_t i = 1; i < cycle.size(); ++i) {
            EXPECT_LT(ContractKey::month_of_code(cycle[i - 1]), ContractKey::month_of_code(cycle[i]))
                << root;
        }
    }
}

TEST(RollCycle, MeasuresHeldMonthsAndIgnoresOneBarStrays) {
    // Four delivery years. The vendor holds Mar and Sep each year (two bars or more), Jun in two of
    // the four years (half: in), Dec in one (a quarter: out), and names a Jan contract for one bar
    // in every year (a stray: never held).
    std::vector<ContractBar> rows;
    std::vector<VendorBar> grid;
    int id = 100;
    auto add = [&](const std::string& raw, const std::string& exp, std::vector<std::string> dates) {
        const std::string sid = std::to_string(id++);
        for (const auto& dt : dates) {
            rows.push_back(bar("XX", raw, sid, dt, exp, 1.0, 1));
            grid.push_back(vbar(dt, sid));
        }
    };
    for (int y = 2020; y <= 2023; ++y) {
        const std::string Y = std::to_string(y);
        const std::string dig = std::to_string(y % 10);
        add("XXF" + dig, Y + "-01-15", {Y + "-01-05"});
        add("XXH" + dig, Y + "-03-15", {Y + "-02-01", Y + "-02-02", Y + "-02-03"});
        if (y <= 2021) add("XXM" + dig, Y + "-06-15", {Y + "-05-01", Y + "-05-04"});
        add("XXU" + dig, Y + "-09-15", {Y + "-08-03", Y + "-08-04"});
        if (y == 2020) add("XXZ" + dig, Y + "-12-15", {Y + "-11-02", Y + "-11-03"});
    }
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok()) << book.error()->what();
    auto m = measure_cycle("XX", grid, book.value(), [](Day) { return std::string("XX"); });
    ASSERT_TRUE(m.is_ok()) << m.error()->what();
    EXPECT_EQ(m.value().years, 4);
    EXPECT_EQ(m.value().held_years[0], 0);  // Jan: strays only
    EXPECT_EQ(m.value().held_years[2], 4);
    EXPECT_EQ(m.value().held_years[5], 2);
    EXPECT_EQ(m.value().held_years[11], 1);
    EXPECT_EQ(m.value().cycle(), "HMU");
    EXPECT_EQ(m.value().near_cut(), "M");
    EXPECT_EQ(m.value().unresolved_bars, 0);
    // An id the book cannot name is counted, not guessed.
    grid.push_back(vbar("2024-01-02", "999"));
    auto m2 = measure_cycle("XX", grid, book.value(), [](Day) { return std::string("XX"); });
    ASSERT_TRUE(m2.is_ok());
    EXPECT_EQ(m2.value().unresolved_bars, 1);
}

TEST(RollCycle, RefusesAHeldContractWithNoExpiration) {
    std::vector<ContractBar> rows = {bar("XX", "XXH0", "1", "2020-02-01", "", 1.0, 1),
                                     bar("XX", "XXH0", "1", "2020-02-02", "", 1.0, 1)};
    std::vector<VendorBar> grid = {vbar("2020-02-01", "1"), vbar("2020-02-02", "1")};
    auto book = ContractBook::build(rows);
    ASSERT_TRUE(book.is_ok());
    EXPECT_TRUE(
        measure_cycle("XX", grid, book.value(), [](Day) { return std::string("XX"); }).is_error());
}
