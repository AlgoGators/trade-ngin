// Carry's second contract (trade-ngin #185, data/carry_quote.hpp): the nearer contract of the root's
// roll cycle when one is alive, else the further one; the quote needs both contracts' own rows on
// the date. Fixtures are Carver's two worked examples (AFTS pages 221 to 223) and the gold cycle.
#include <gtest/gtest.h>

#include "contract_rolls_test_util.hpp"
#include "trade_ngin/data/carry_quote.hpp"
#include "trade_ngin/data/roll_cycle.hpp"

using namespace trade_ngin;
using namespace trade_ngin::contract_rolls;
using contract_rolls_test::bar;
using contract_rolls_test::d;

namespace {

ContractKey key(const std::string& root, const std::string& raw, const std::string& expiration) {
    return ContractKey::make(root, raw, d(expiration)).value();
}

}  // namespace

TEST(CarryQuote, TheHeldIsNotTheFrontReadsTheNearerContract) {
    // WTI crude, holding December 2022 (Carver): November 62.61, December 62.09. October is
    // expired by the date, January is further: neither is read.
    const auto book = ContractBook::build({
                                              bar("CL", "CLV2", "1", "2021-09-01", "2022-09-20", 63.0, 10),
                                              bar("CL", "CLX2", "2", "2021-11-15", "2022-10-20", 62.61, 10),
                                              bar("CL", "CLZ2", "3", "2021-11-15", "2022-11-21", 62.09, 10),
                                              bar("CL", "CLF3", "4", "2021-11-15", "2022-12-19", 61.70, 10),
                                          })
                          .value();
    const auto held = key("CL", "CLZ2", "2022-11-21");
    const auto q = carry_quote(book, held, d("2021-11-15"), roll_cycle("CL").value());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->held_close, 62.09);
    EXPECT_EQ(q->other_close, 62.61);
    EXPECT_EQ(q->months, -1);
    EXPECT_EQ(q->held_symbol, "CLZ2");
    EXPECT_EQ(q->other_symbol, "CLX2");
}

TEST(CarryQuote, TheHeldIsTheFrontReadsTheFurtherContract) {
    // S&P 500 micro, holding December 2021, the front (Carver): March 4572.00, December 4578.50.
    // June is listed too; the first further contract is read.
    const auto book = ContractBook::build({
                                              bar("MES", "MESZ1", "1", "2021-11-10", "2021-12-17", 4578.50, 10),
                                              bar("MES", "MESH2", "2", "2021-11-10", "2022-03-18", 4572.00, 10),
                                              bar("MES", "MESM2", "3", "2021-11-10", "2022-06-17", 4560.00, 10),
                                          })
                          .value();
    const auto q = carry_quote(book, key("MES", "MESZ1", "2021-12-17"), d("2021-11-10"),
                               roll_cycle("MES").value());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->held_close, 4578.50);
    EXPECT_EQ(q->other_close, 4572.00);
    EXPECT_EQ(q->months, 3);
    EXPECT_EQ(q->other_symbol, "MESH2");
}

TEST(CarryQuote, OnlyTheRootsCycleMonthsAreNeighbours) {
    // Gold's cycle is G J M Q Z. Holding December, the October and November serial months are alive
    // and print, but they are not cycle months: the held contract is the cycle front, so the
    // further cycle contract (February) is read, not November.
    const auto book = ContractBook::build({
                                              bar("GC", "GCV5", "1", "2025-10-01", "2025-10-29", 3800.0, 5),
                                              bar("GC", "GCX5", "2", "2025-10-01", "2025-11-25", 3810.0, 5),
                                              bar("GC", "GCZ5", "3", "2025-10-01", "2025-12-29", 3820.0, 500),
                                              bar("GC", "GCF6", "4", "2025-10-01", "2026-01-28", 3830.0, 5),
                                              bar("GC", "GCG6", "5", "2025-10-01", "2026-02-25", 3840.0, 50),
                                          })
                          .value();
    const auto held = key("GC", "GCZ5", "2025-12-29");
    const auto neighbour = carry_neighbour(book, held, d("2025-10-01"), roll_cycle("GC").value());
    ASSERT_NE(neighbour.contract, nullptr);
    EXPECT_EQ(neighbour.contract->raw_symbol, "GCG6");
    EXPECT_EQ(neighbour.months, 2);
    // Any listed month would have taken the November serial: the cycle is what decides.
    const auto any_month = carry_neighbour(book, held, d("2025-10-01"), "FGHJKMNQUVXZ");
    ASSERT_NE(any_month.contract, nullptr);
    EXPECT_EQ(any_month.contract->raw_symbol, "GCX5");
}

TEST(CarryQuote, ANearerContractWithNoRowLeavesTheDayWithoutAQuote) {
    // Holding June gold while April is alive: April has no row on 2025-03-11. The day has no quote;
    // it does not switch to the further contract (August prints). On 2025-03-10 April prints.
    const auto book = ContractBook::build({
                                              bar("GC", "GCJ5", "1", "2025-03-10", "2025-04-28", 2900.0, 5),
                                              bar("GC", "GCM5", "2", "2025-03-10", "2025-06-26", 2920.0, 500),
                                              bar("GC", "GCM5", "2", "2025-03-11", "2025-06-26", 2925.0, 500),
                                              bar("GC", "GCQ5", "3", "2025-03-10", "2025-08-27", 2940.0, 50),
                                              bar("GC", "GCQ5", "3", "2025-03-11", "2025-08-27", 2945.0, 50),
                                          })
                          .value();
    const auto held = key("GC", "GCM5", "2025-06-26");
    const std::string cycle = roll_cycle("GC").value();
    EXPECT_EQ(carry_neighbour(book, held, d("2025-03-11"), cycle).contract->raw_symbol, "GCJ5");
    EXPECT_FALSE(carry_quote(book, held, d("2025-03-11"), cycle).has_value());
    const auto q = carry_quote(book, held, d("2025-03-10"), cycle);
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->other_symbol, "GCJ5");
    EXPECT_EQ(q->months, -2);
}

TEST(CarryQuote, TheHeldContractWithoutItsOwnRowHasNoQuote) {
    const auto book = ContractBook::build({
                                              bar("ZN", "ZNZ5", "1", "2025-11-03", "2025-12-19", 112.0, 5),
                                              bar("ZN", "ZNH6", "2", "2025-11-03", "2026-03-20", 111.5, 5),
                                              bar("ZN", "ZNH6", "2", "2025-11-04", "2026-03-20", 111.6, 5),
                                          })
                          .value();
    // Holding December on 11-04: it has no row that day (a carried close in the bar source).
    EXPECT_FALSE(carry_quote(book, key("ZN", "ZNZ5", "2025-12-19"), d("2025-11-04"),
                             roll_cycle("ZN").value())
                     .has_value());
}

TEST(CarryQuote, ExpiredUnlistedAndUnresolvedContractsAreNotNeighbours) {
    const auto book = ContractBook::build({
                                              // expired before the date: not a nearer contract
                                              bar("ZN", "ZNU5", "1", "2025-09-15", "2025-09-19", 113.0, 5),
                                              bar("ZN", "ZNZ5", "2", "2025-09-15", "2025-12-19", 112.0, 5),
                                              bar("ZN", "ZNZ5", "2", "2025-09-22", "2025-12-19", 112.1, 5),
                                              // no expiration: passed over
                                              bar("ZN", "ZNH6", "3", "2025-09-22", "", 111.5, 5),
                                              // first row after the date: not listed yet
                                              bar("ZN", "ZNM6", "4", "2025-10-01", "2026-06-18", 111.0, 5),
                                          })
                          .value();
    const auto held = key("ZN", "ZNZ5", "2025-12-19");
    const auto none = carry_neighbour(book, held, d("2025-09-22"), roll_cycle("ZN").value());
    EXPECT_EQ(none.contract, nullptr);
    EXPECT_FALSE(carry_quote(book, held, d("2025-09-22"), roll_cycle("ZN").value()).has_value());
    // On 09-15 September is alive (expires 09-19): it is the nearer contract.
    const auto nearer = carry_neighbour(book, held, d("2025-09-15"), roll_cycle("ZN").value());
    ASSERT_NE(nearer.contract, nullptr);
    EXPECT_EQ(nearer.contract->raw_symbol, "ZNU5");
    EXPECT_EQ(nearer.months, -3);
}

TEST(CarryQuote, OnlyTheHeldContractsRootIsSearched) {
    // A micro before 2026-02-22 holds the full-size contract: the book has both roots; the
    // neighbour comes from the held contract's own root.
    const auto book = ContractBook::build({
                                              bar("ES", "ESZ5", "1", "2025-11-03", "2025-12-19", 6800.0, 5),
                                              bar("ES", "ESH6", "2", "2025-11-03", "2026-03-20", 6850.0, 5),
                                              bar("MES", "MESH6", "3", "2025-11-03", "2026-03-20", 6851.0, 5),
                                          })
                          .value();
    const auto q = carry_quote(book, key("ES", "ESZ5", "2025-12-19"), d("2025-11-03"),
                               roll_cycle("ES").value());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->other_symbol, "ESH6");
    EXPECT_EQ(q->other_close, 6850.0);
}
