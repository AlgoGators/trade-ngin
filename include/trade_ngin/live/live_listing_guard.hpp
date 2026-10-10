// include/trade_ngin/live/live_listing_guard.hpp
#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/listing_dates.hpp"

namespace trade_ngin {

/**
 * @brief The live futures run's listing-date guard (portfolio.json's optional listing_dates)
 *
 * A live run never trades a predecessor contract: the backtest hands a predecessor the rows stored
 * under its listed contract's symbol (data/listing_dates.hpp), a live run does not. So with
 * contracts declared a live run REFUSES, before anything of the day is stored, when
 *   - a predecessor symbol is in the run's universe on a signal date on or after the listing date
 *     of its listed contract (the book trades the listed contract from that date),
 *   - a stored Day T-1 position is in a predecessor symbol on such a date, or
 *   - the signal date is BEFORE the listing date of a listed contract in the universe (the run
 *     would have to trade the predecessor).
 * One line per finding, each starting "LISTING_DATE_REFUSAL"; none when nothing is declared or
 * nothing is found. `signal_date` is Day T-1 as YYYY-MM-DD; `universe` is the symbol list as the
 * database gives it, before any symbol is filtered by name; a stored row of quantity 0 is no
 * position. Whose stored book a run continues is not judged here (live/stored_book_ownership.hpp).
 */
inline std::vector<std::string> live_listing_refusals(
    const std::vector<ListedContract>& contracts, const std::vector<std::string>& universe,
    const std::unordered_map<std::string, Position>& stored_book, const std::string& signal_date) {
    std::vector<std::string> lines;
    for (const auto& contract : contracts) {
        const bool listed = !(signal_date < contract.listed);  // YYYY-MM-DD compares as text
        for (const auto& symbol : universe) {
            const std::string root = ListingDates::root_of(symbol);
            if (listed && root == contract.before) {
                lines.push_back(
                    "LISTING_DATE_REFUSAL symbol " + symbol + " is in the run's universe on signal date " +
                    signal_date + ", on or after the listing date " + contract.listed + " of " +
                    contract.symbol + ", the contract the book trades from that date. Refusing to "
                    "run: a predecessor contract is not traded on or after its listed contract's "
                    "listing date. Remove " + symbol + "'s rows from futures_data.ohlcv_1d, or "
                    "correct portfolio.json listing_dates.");
            }
            if (!listed && root == contract.symbol) {
                lines.push_back(
                    "LISTING_DATE_REFUSAL signal date " + signal_date + " is before the listing date " +
                    contract.listed + " of " + symbol + ", which is in the run's universe. Refusing "
                    "to run: a live run does not trade the predecessor contract " + contract.before +
                    ". Run a date on or after the listing date, or correct portfolio.json "
                    "listing_dates.");
            }
        }
        if (!listed) continue;
        std::vector<std::string> held;
        for (const auto& [symbol, position] : stored_book) {
            if (position.quantity.as_double() != 0.0 &&
                ListingDates::root_of(symbol) == contract.before) {
                held.push_back(symbol);
            }
        }
        std::sort(held.begin(), held.end());
        for (const auto& symbol : held) {
            lines.push_back(
                "LISTING_DATE_REFUSAL stored position " + symbol + " (quantity " +
                std::to_string(stored_book.at(symbol).quantity.as_double()) + ", Day T-1 " +
                signal_date + ") is in a predecessor contract on or after the listing date " +
                contract.listed + " of " + contract.symbol + ". Refusing to run: a predecessor "
                "contract is not held on or after its listed contract's listing date. Correct "
                "that trading.positions row, or correct portfolio.json listing_dates.");
        }
    }
    return lines;
}

}  // namespace trade_ngin
