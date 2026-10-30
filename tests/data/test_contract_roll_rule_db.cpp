// A worked example of the per-contract roll rule on the real bars of a test clone of the database
// (read only, through TRADE_NGIN_TEST_DSN; skips without it or when the expiration column is not
// filled):
//
//   | Bar                  | GCQ6 (held)     | GCZ6 (target)    | What the rule does                       |
//   | Fri 07-24            | 77,474          | 8,982            | Window opens (D* = first notice 07-31)    |
//   | Mon 07-27, Tue 07-28 | 77,116 / 77,452 | 13,313 / 23,314  | No crossover: hold                        |
//   | Wed 07-29            | 22,870          | 117,650          | Crossover, and the last actionable bar    |
//   | Fri 07-31            | 435             | 99,065           | The vendor switches; today's engine rolls |
//
// GCU6 (September) and GCV6 (October) are never held: not in the cycle.
#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/contract_roll_rule.hpp"

using namespace trade_ngin::contract_rolls;
using contract_rolls_test::d;

namespace {

struct Loaded {
    std::vector<VendorBar> grid;
    std::unique_ptr<ContractBook> book;
};

}  // namespace

class ContractRollRuleDbTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::string c = contract_rolls_test::dsn();
        if (c.empty()) GTEST_SKIP() << "TRADE_NGIN_TEST_DSN not set";
        try {
            conn_ = std::make_unique<pqxx::connection>(c);
            pqxx::read_transaction t(*conn_);
            const auto r = t.exec(
                "SELECT count(*) FILTER (WHERE expiration IS NULL), count(*) FROM "
                "futures_data.contract_ohlcv_1d WHERE root = 'GC' AND date >= '2026-06-01'");
            if (r[0][1].as<int64_t>() == 0 || r[0][0].as<int64_t>() > 0) {
                GTEST_SKIP() << "no GC contract bars with an expiration";
            }
        } catch (const std::exception& e) {
            GTEST_SKIP() << "database unreachable: " << e.what();
        }
    }
    Loaded load(const std::string& root, const std::string& from, const std::string& to) {
        pqxx::read_transaction t(*conn_);
        Loaded l;
        l.grid = read_vendor_grid(t, root + ".v.0", d(from), d(to));
        l.book = std::make_unique<ContractBook>(
            ContractBook::build(read_contract_bars(t, {root}, d(from), d(to))).value());
        return l;
    }
    std::unique_ptr<pqxx::connection> conn_;
};

TEST_F(ContractRollRuleDbTest, GcAugust2026RollsOnTheCrossoverOfWednesday0729) {
    Loaded l = load("GC", "2026-06-01", "2026-08-14");
    const TradingCalendar cal = contract_rolls_test::shipped_calendar();
    RuleInputs in;
    in.root = "GC";
    in.grid = l.grid;
    in.book = l.book.get();
    in.cal = &cal;
    in.cycle = roll_cycle("GC").value();
    auto r = apply_roll_rule(in);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    std::map<std::string, const HeldDay*> by;
    for (const auto& day : r.value().days) by[ymd(day.date)] = &day;
    auto vol = [&](const std::string& raw, const std::string& date) {
        for (const Contract* c : l.book->printing_on("GC", d(date))) {
            if (c->raw_symbol == raw) return c->bar_on(d(date))->volume;
        }
        return int64_t{-1};
    };
    EXPECT_EQ(vol("GCQ6", "2026-07-24"), 77474);
    EXPECT_EQ(vol("GCZ6", "2026-07-24"), 8982);
    EXPECT_EQ(vol("GCQ6", "2026-07-29"), 22870);
    EXPECT_EQ(vol("GCZ6", "2026-07-29"), 117650);
    for (const char* hold : {"2026-07-24", "2026-07-27", "2026-07-28"}) {
        ASSERT_TRUE(by.count(hold)) << hold;
        EXPECT_EQ(by[hold]->held.raw_symbol, "GCQ6") << hold;
        EXPECT_FALSE(by[hold]->roll) << hold;
    }
    const HeldDay& roll = *by.at("2026-07-29");
    EXPECT_TRUE(roll.roll);
    EXPECT_EQ(roll.trigger, RollTrigger::CROSSOVER);
    EXPECT_EQ(roll.old->raw_symbol, "GCQ6");
    EXPECT_EQ(roll.held.raw_symbol, "GCZ6");
    EXPECT_EQ(roll.held.delivery_year, 2026);
    EXPECT_TRUE(roll.old_close.has_value());
    // The vendor switches on Friday 07-31: the held contract is already GCZ6; no second roll.
    EXPECT_EQ(by.at("2026-07-31")->vendor->raw_symbol, "GCZ6");
    EXPECT_FALSE(by.at("2026-07-31")->roll);
    int rolls_in_july = 0;
    for (const auto& day : r.value().days) {
        if (day.roll && day.date >= d("2026-07-01") && day.date <= d("2026-07-31")) ++rolls_in_july;
    }
    EXPECT_EQ(rolls_in_july, 1);
}

TEST_F(ContractRollRuleDbTest, GcNeverHoldsSeptemberOrOctoberOverTheWholeHistory) {
    Loaded l = load("GC", "2010-06-06", "2026-10-02");
    const TradingCalendar cal = contract_rolls_test::shipped_calendar();
    RuleInputs in;
    in.root = "GC";
    in.grid = l.grid;
    in.book = l.book.get();
    in.cal = &cal;
    in.cycle = roll_cycle("GC").value();
    auto r = apply_roll_rule(in);
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    int vendor_off_cycle_days = 0;
    for (const auto& day : r.value().days) {
        EXPECT_TRUE(in_cycle(in.cycle, day.held.delivery_month)) << ymd(day.date) << " "
                                                                  << day.held.raw_symbol;
        if (day.vendor && !in_cycle(in.cycle, day.vendor->delivery_month)) ++vendor_off_cycle_days;
    }
    // The vendor itself names off-cycle months on some bars; the rule never holds them.
    EXPECT_GT(vendor_off_cycle_days, 0);
}
