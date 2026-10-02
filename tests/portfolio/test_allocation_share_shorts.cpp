// D9 (LOOP_SPEC v6.1 sections 3 and 5.4; T-ROLLX commit 3): the share test that zeroed a
// single-sign negative total is removed. A sleeve's share is computed on any non-zero total, so a
// short target is distributed like a long one; a zero total still distributes nothing.
#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include <vector>
#include "trade_ngin/portfolio/allocation_split.hpp"

using namespace trade_ngin;

TEST(AllocationShareShorts, ASingleSleevesShortTargetIsStoredShort) {
    const auto d = distribute_optimizer_contracts(-3.0, {{"TREND", -3.0}});
    ASSERT_EQ(d.stored.size(), 1u);
    EXPECT_EQ(d.stored[0], -3) << "the share on a negative total is 1, not 0";
    EXPECT_EQ(d.book, -3);
    EXPECT_DOUBLE_EQ(d.quota[0], -3.0);
}

TEST(AllocationShareShorts, TwoShortSleevesSplitTheShortBook) {
    const auto d = distribute_optimizer_contracts(-3.0, {{"FAST", -0.9}, {"TREND", -2.1}});
    ASSERT_EQ(d.stored.size(), 2u);
    EXPECT_EQ(d.stored[0] + d.stored[1], -3);
    EXPECT_EQ(d.stored[1], -2);
    EXPECT_EQ(d.stored[0], -1);
    EXPECT_NEAR(d.quota[0], -0.9, 1e-12);
    EXPECT_NEAR(d.quota[1], -2.1, 1e-12);
}

TEST(AllocationShareShorts, AZeroTotalStillDistributesNothingAndALongBookIsUnchanged) {
    const auto z = distribute_optimizer_contracts(2.0, {{"A", 0.0}, {"B", 0.0}});
    EXPECT_EQ(z.stored[0] + z.stored[1], 0);
    const auto l = distribute_optimizer_contracts(3.0, {{"FAST", 0.9}, {"TREND", 2.1}});
    EXPECT_EQ(l.stored[0], 1);
    EXPECT_EQ(l.stored[1], 2);
}
