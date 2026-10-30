// include/trade_ngin/data/carry_quote.hpp
#pragma once

#include <optional>
#include <string>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/contract_bars.hpp"

namespace trade_ngin::contract_rolls {

/**
 * @brief Carry's second contract (trade-ngin #185; C1 step 0, the lead's answer of 2026-10-26: the
 *        neighbour is taken among the months of the root's roll cycle, roll_cycle_table()).
 *
 * On `day`, for the held contract `held`:
 *   - NEARER: the contract of held.root whose month is in `cycle`, which expires before the held one
 *     and is alive on the day (its first row is on or before the day, its expiration on or after
 *     it); the latest-expiring such contract.
 *   - FURTHER, when no nearer contract is alive (the held contract is the front): the first
 *     contract of held.root whose month is in `cycle`, which expires after the held one and is listed
 *     on the day (its first row is on or before the day).
 * The side is decided by which contracts are alive, never by who printed: a nearer contract with no
 * row on the day leaves the day without a quote (the last known raw carry stands, HD's Q4), it does
 * not switch the day to the further contract. Contracts with no expiration are passed over. nullptr
 * when there is no neighbour at all.
 */
struct CarryNeighbour {
    const Contract* contract{nullptr};
    int months{0};  ///< the neighbour's delivery month less the held one's; < 0 nearer, > 0 further
};
CarryNeighbour carry_neighbour(const ContractBook& book, const ContractKey& held, Day day,
                               const std::string& cycle);

/**
 * @brief The day's carry quote: the held contract's own close and its neighbour's close on `day`.
 *        nullopt when there is no neighbour, or when either contract has no row of its own that day.
 */
std::optional<CarryQuote> carry_quote(const ContractBook& book, const ContractKey& held, Day day,
                                      const std::string& cycle);

}  // namespace trade_ngin::contract_rolls
