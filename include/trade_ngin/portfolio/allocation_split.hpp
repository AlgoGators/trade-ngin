// include/trade_ngin/portfolio/allocation_split.hpp
//
// The post-optimizer distribution of one symbol over the sleeves that asked for it
// (PortfolioManager::optimize_positions). Ledger N2: a sleeve sizes its contracts on its own
// capital slice (capital x allocation), so its contracts are already contracts of the account's
// book, and the account holds the sum of the sleeves' contracts (the book apply_risk_management
// gates). The optimizer therefore works in account weight, one contract weighing
// w = notional per contract / total capital, and the allocation is applied nowhere here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace trade_ngin {

/// One sleeve's part of a symbol in the optimizer's aggregate: its target contracts x w.
struct SleeveContribution {
    std::string strategy_id;
    double contribution;
};

/// The distribution of one symbol, in the order of the sleeves given.
struct SleeveDistribution {
    std::vector<double> quota;                 ///< each sleeve's contracts, unrounded
    std::vector<int64_t> stored;               ///< the largest-remainder split; sums to book
    std::vector<int64_t> per_sleeve_rounding;  ///< round(round(answer) x share), for the log only
    int64_t book = 0;                          ///< the sum of stored
};

/// optimizer_contracts is the optimizer's answer for the symbol in account contracts (its weight
/// divided by w), unrounded. Each sleeve's quota is that answer times its share of the
/// contributions (0 when the contributions do not sum above 1e-8); the book is the quotas'
/// sum rounded ONCE, half away from zero; the stored integers are the largest-remainder
/// (Hamilton) split of the book: the floor of each quota plus one contract for each of the
/// largest remainders, ties to the smaller strategy_id, so they sum to the book exactly.
SleeveDistribution distribute_optimizer_contracts(double optimizer_contracts,
                                                  const std::vector<SleeveContribution>& sleeves);

}  // namespace trade_ngin
