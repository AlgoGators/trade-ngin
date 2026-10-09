// tests/portfolio/test_allocation_distribution.cpp
//
// Ledger N2, pure tests of the post-optimizer distribution (distribute_optimizer_contracts in
// include/trade_ngin/portfolio/allocation_split.hpp): given the optimizer's answer for one symbol
// in ACCOUNT contracts and each sleeve's contribution (its target contracts x the account weight
// of one contract), each sleeve's quota is answer x its share, the book is round(answer) (half
// away from zero), and the stored sleeve integers are the largest-remainder split of the book
// (ties to the smaller strategy id). The allocation appears nowhere: a sleeve sized its contracts
// on its own capital slice, so they are the account's contracts already.
//
// The function is new with the fix, so this file cannot build on the parent; its RED
// counterpart is tests/portfolio/test_optimizer_allocation_once.cpp, which drives a real
// PortfolioManager through the same cases. Each case states what the parent stored,
//     parent quota_s = answer x (q_s a_s / sum(q a)) / a_s,
// with the answer then counted in allocation-weighted contracts.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "trade_ngin/portfolio/allocation_split.hpp"

using namespace trade_ngin;

namespace {

// Any positive weight per contract: the distribution reads only the ratios of contributions.
constexpr double kWeightPerContract = 0.0873;

std::vector<SleeveContribution> sleeves_of(
    const std::vector<std::pair<std::string, double>>& target_contracts) {
    std::vector<SleeveContribution> out;
    for (const auto& [id, q] : target_contracts) out.push_back({id, q * kWeightPerContract});
    return out;
}

int64_t sum_of(const std::vector<int64_t>& v) {
    int64_t s = 0;
    for (auto x : v) s += x;
    return s;
}

}  // namespace

// CONSERVATIVE's and the futures backtest's shape: one sleeve. The quota is the answer and the
// stored integer is round(answer), which is what the parent stored at allocation 1.0.
TEST(AllocationDistribution, OneSleeveStoresTheRoundedAnswer) {
    const auto one = sleeves_of({{"TREND_FOLLOWING", 3.0}});
    for (double answer : {0.0, 0.2, 0.49, 0.5, 1.0, 1.29, 1.5, 2.0, 2.5, 3.7, 7.0}) {
        const auto d = distribute_optimizer_contracts(answer, one);
        ASSERT_EQ(d.stored.size(), 1u);
        EXPECT_EQ(d.stored[0], std::llround(answer)) << "answer " << answer;
        EXPECT_EQ(d.book, std::llround(answer)) << "answer " << answer;
        EXPECT_DOUBLE_EQ(d.quota[0], answer);
        EXPECT_EQ(d.per_sleeve_rounding[0], d.stored[0]) << "no ALLOCATION_SPLIT line on one sleeve";
    }
}

// BASE's 6C on 2026-04-24: TF asks 1, FAST asks 0.292997, the optimizer answers 1.
//   parent: the answer was 1 allocation-weighted contract of an aggregate 0.787899; quotas 1.269199
//           / 0.371871, book round(1.641070) = 2, TF 1 / FAST 1.
//   fixed:  quotas 1 / 1.292997 = 0.773397 and 0.226603; book 1; TF 1 / FAST 0.
TEST(AllocationDistribution, TheSixCBookOfTwentyFourAprilStoresOne) {
    const auto d = distribute_optimizer_contracts(
        1.0, sleeves_of({{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", 0.292997}}));
    EXPECT_NEAR(d.quota[0], 0.773397, 1e-6);
    EXPECT_NEAR(d.quota[1], 0.226603, 1e-6);
    EXPECT_EQ(d.book, 1);
    EXPECT_EQ(d.stored[0], 1);
    EXPECT_EQ(d.stored[1], 0);
}

// A symbol only FAST holds (TF's target 0, as on 6A and MNQ). An answer of 0.9 contracts is one
// contract of book.
//   parent: 0.9 x 1 / 0.3 = 3 contracts (and an answer of 1, one greedy step, 3.33, stored 3).
//   fixed:  quota 0.9, book 1, FAST 1; an answer of 1 stores 1; TF, with no share, stores 0.
TEST(AllocationDistribution, AFastOnlyAnswerOfNineTenthsStoresOneNotThree) {
    const auto sleeves = sleeves_of({{"TREND_FOLLOWING", 0.0}, {"TREND_FOLLOWING_FAST", 0.441733}});
    auto d = distribute_optimizer_contracts(0.9, sleeves);
    EXPECT_DOUBLE_EQ(d.quota[0], 0.0);
    EXPECT_DOUBLE_EQ(d.quota[1], 0.9);
    EXPECT_EQ(d.stored[0], 0);
    EXPECT_EQ(d.stored[1], 1);
    EXPECT_EQ(d.book, 1);

    d = distribute_optimizer_contracts(1.0, sleeves);
    EXPECT_EQ(d.stored[0], 0);
    EXPECT_EQ(d.stored[1], 1);
}

// The split's invariants over a sweep of answers (a grid that avoids the exact halves, where
// the sum of the quotas can sit one ulp either side of x.5) and of sleeve mixes, two and three
// sleeves: the book is round(answer); the stored integers sum to the book exactly; each sleeve is
// within one contract of its quota and never below zero; a sleeve with no contribution stores 0.
TEST(AllocationDistribution, TheStoredIntegersSumToTheRoundedAnswer) {
    const std::vector<std::vector<std::pair<std::string, double>>> mixes = {
        {{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", 0.292997}},
        {{"TREND_FOLLOWING", 2.0}, {"TREND_FOLLOWING_FAST", 0.878282}},
        {{"TREND_FOLLOWING", 3.0}, {"TREND_FOLLOWING_FAST", 2.055461}},
        {{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", 1.0}},
        {{"TREND_FOLLOWING", 0.0}, {"TREND_FOLLOWING_FAST", 2.663504}},
        {{"A_SLEEVE", 1.0}, {"B_SLEEVE", 1.0}, {"C_SLEEVE", 1.0}},
        {{"A_SLEEVE", 0.4}, {"B_SLEEVE", 2.7}, {"C_SLEEVE", 0.9}},
    };
    for (const auto& mix : mixes) {
        const auto sleeves = sleeves_of(mix);
        for (int k = 0; k <= 800; ++k) {
            const double answer = 0.001 + 0.01 * k;
            const auto d = distribute_optimizer_contracts(answer, sleeves);
            ASSERT_EQ(d.stored.size(), mix.size());
            EXPECT_EQ(d.book, std::llround(answer)) << "answer " << answer;
            EXPECT_EQ(sum_of(d.stored), d.book) << "answer " << answer;
            for (size_t s = 0; s < mix.size(); ++s) {
                EXPECT_LT(std::fabs(static_cast<double>(d.stored[s]) - d.quota[s]), 1.0)
                    << mix[s].first << " answer " << answer;
                EXPECT_GE(d.stored[s], 0) << mix[s].first << " answer " << answer;
                if (mix[s].second == 0.0) EXPECT_EQ(d.stored[s], 0);
            }
        }
    }
}

// Ties: two sleeves asking the same, an answer of 1: quotas 0.5 / 0.5, book 1, and the one
// contract goes to the smaller strategy id whatever the order the sleeves arrive in.
TEST(AllocationDistribution, ATieGoesToTheSmallerStrategyId) {
    auto d = distribute_optimizer_contracts(1.0, sleeves_of({{"SLEEVE_Z", 1.0}, {"SLEEVE_A", 1.0}}));
    EXPECT_EQ(d.stored[0], 0);
    EXPECT_EQ(d.stored[1], 1);

    d = distribute_optimizer_contracts(1.0, sleeves_of({{"SLEEVE_A", 1.0}, {"SLEEVE_Z", 1.0}}));
    EXPECT_EQ(d.stored[0], 1);
    EXPECT_EQ(d.stored[1], 0);
}

// T-7b-2 8b commit 2, the opposed-sleeve split (HD 2026-09-25 item 23). When the sleeves' targets
// have OPPOSITE signs, a share of the contributions is not a fraction of the answer: it exceeds 1
// or is negative, so answer x share amplifies (targets +1.0 / -0.8 with an answer of 1 stored
// +5 / -4) and a cancelling total (the 1e-8 guard) flattens both sleeves. HD's ruling: opposite
// sleeves are kept gross per sleeve while the account holds the net, and only the optimizer's
// DEVIATION from the net target is split, weighted by target size:
//     quota_i = t_i + (answer - T) x |t_i| / sum |t_j|,   T = sum t_j,
// which never moves a sleeve by more than |answer - T|. On same-sign targets |t_i| / sum |t_j| =
// t_i / T and the quota is answer x share exactly, so that branch keeps today's expression.
// These cases pass the targets in contracts (a weight of 1 per contract), which is what the
// function reads on the opposed branch when no separate contract count is given.

// Targets that cancel: TF +1, FAST -1, the account's net target 0, the optimizer answers 0.
//   parent: sum of contributions 0, the guard gives both a share of 0: TF 0, FAST 0 (a crossing
//           pair of fills that flattens both sleeves' attribution).
//   fixed:  quotas +1 / -1 (no deviation): TF +1, FAST -1, book 0.
TEST(AllocationDistribution, OpposedTargetsThatCancelAreKeptGrossPerSleeve) {
    const auto d = distribute_optimizer_contracts(
        0.0, {{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", -1.0}});
    EXPECT_EQ(d.book, 0);
    EXPECT_EQ(d.stored[0], 1);
    EXPECT_EQ(d.stored[1], -1);
    EXPECT_DOUBLE_EQ(d.quota[0], 1.0);
    EXPECT_DOUBLE_EQ(d.quota[1], -1.0);
}

// Near-cancel with a held answer: TF +1.0, FAST -0.8 (T = 0.2), the optimizer keeps 1.
//   parent: shares 1.0/0.2 = 5 and -0.8/0.2 = -4: TF +5, FAST -4 (gross 9 for a net of 1).
//   fixed:  deviation 0.8 split 1.0 : 0.8 of 1.8: quotas 1.444 / -0.444, book 1; floors 1 / -1,
//           the extra contract to the larger remainder (FAST 0.556): TF 1, FAST 0.
TEST(AllocationDistribution, OpposedTargetsAreNeverAmplified) {
    const auto d = distribute_optimizer_contracts(
        1.0, {{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", -0.8}});
    EXPECT_EQ(d.book, 1);
    EXPECT_EQ(d.stored[0], 1);
    EXPECT_EQ(d.stored[1], 0);
    EXPECT_NEAR(d.quota[0], 1.0 + 0.8 / 1.8, 1e-12);
    EXPECT_NEAR(d.quota[1], -0.8 + 0.8 * 0.8 / 1.8, 1e-12);
}

// A net-short target on opposed sleeves: TF +1, FAST -1.5 (T = -0.5); the long-only optimizer
// answers 0.
//   parent: the sum of contributions is below the guard: TF 0, FAST 0.
//   fixed:  deviation +0.5 split 1 : 1.5 of 2.5: quotas 1.2 / -1.2, book 0: TF +1, FAST -1.
TEST(AllocationDistribution, ANetShortOpposedTargetKeepsBothSleeves) {
    const auto d = distribute_optimizer_contracts(
        0.0, {{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", -1.5}});
    EXPECT_EQ(d.book, 0);
    EXPECT_EQ(d.stored[0], 1);
    EXPECT_EQ(d.stored[1], -1);
}

// With no deviation the opposed targets are stored as they are (the parent agrees: shares 1.5 and
// -0.5 of an answer of 2 are 3 and -1). A control, green on both.
TEST(AllocationDistribution, OpposedTargetsTheOptimizerKeepsAreStoredAsAsked) {
    const auto d = distribute_optimizer_contracts(
        2.0, {{"TREND_FOLLOWING", 3.0}, {"TREND_FOLLOWING_FAST", -1.0}});
    EXPECT_EQ(d.book, 2);
    EXPECT_EQ(d.stored[0], 3);
    EXPECT_EQ(d.stored[1], -1);
}

// The opposed rule's bound on a sweep: every quota within |answer - T| of its sleeve's target, the
// stored integers sum to round(answer), each within one contract of its quota.
TEST(AllocationDistribution, OpposedQuotasMoveEachSleeveByAtMostTheDeviation) {
    for (double tf : {0.3, 1.0, 2.0, 4.6}) {
        for (double fast : {-0.2, -0.8, -1.0, -2.9}) {
            for (int k = 0; k <= 60; ++k) {
                const double answer = 0.013 + 0.1 * k;
                const auto d = distribute_optimizer_contracts(
                    answer, {{"TREND_FOLLOWING", tf}, {"TREND_FOLLOWING_FAST", fast}});
                const double dev = std::fabs(answer - (tf + fast));
                const std::string at = std::to_string(tf) + " " + std::to_string(fast) + " " +
                                       std::to_string(answer);
                EXPECT_LE(std::fabs(d.quota[0] - tf), dev + 1e-12) << at;
                EXPECT_LE(std::fabs(d.quota[1] - fast), dev + 1e-12) << at;
                EXPECT_EQ(sum_of(d.stored), std::llround(answer)) << at;
                for (size_t s = 0; s < 2; ++s)
                    EXPECT_LT(std::fabs(static_cast<double>(d.stored[s]) - d.quota[s]), 1.0);
            }
        }
    }
}

// Same-sign targets are untouched to the last bit: the quota is answer x (contribution / total),
// the parent's expression, on every mix and answer of the sweep above (a guard, green on both).
TEST(AllocationDistribution, SameSignTargetsKeepTheParentsQuotaBitForBit) {
    const std::vector<std::vector<std::pair<std::string, double>>> mixes = {
        {{"TREND_FOLLOWING", 1.0}, {"TREND_FOLLOWING_FAST", 0.292997}},
        {{"TREND_FOLLOWING", 0.0}, {"TREND_FOLLOWING_FAST", 2.663504}},
        {{"A_SLEEVE", 0.4}, {"B_SLEEVE", 2.7}, {"C_SLEEVE", 0.9}},
    };
    for (const auto& mix : mixes) {
        const auto sleeves = sleeves_of(mix);
        double total = 0.0;
        for (const auto& s : sleeves) total += s.contribution;
        for (int k = 0; k <= 800; ++k) {
            const double answer = 0.001 + 0.01 * k;
            const auto d = distribute_optimizer_contracts(answer, sleeves);
            for (size_t s = 0; s < sleeves.size(); ++s) {
                const double share = sleeves[s].contribution / total;
                const double expected = share == 0.0 ? 0.0 : answer * share;
                EXPECT_EQ(d.quota[s], expected) << mix[s].first << " answer " << answer;
            }
        }
    }
}
