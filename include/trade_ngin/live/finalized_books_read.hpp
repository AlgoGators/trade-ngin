// include/trade_ngin/live/finalized_books_read.hpp
#pragma once

#include <string>
#include <unordered_map>

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/live/sleeve_seeding.hpp"

namespace trade_ngin {

/// A stored book per sleeve: sleeve name -> symbol -> row.
using SleeveBooks = std::unordered_map<std::string, std::unordered_map<std::string, Position>>;

/**
 * @brief The Day T-1 books as stored AFTER the run's finalize write, for what the run prints of
 *        yesterday: the email's "Yesterday's Finalized Position Results" table and the attached
 *        <T-1>_positions_asof_<T>.csv.
 *
 * The futures runners load the stored T-1 books early, because the no-session hold needs the
 * stored book before the rebalance. Those rows were written by yesterday's run with the realized
 * P&L at its placeholder 0; this run's finalize writes the settled figure. What is printed of
 * yesterday is therefore read back after that write, one sleeve at a time, with the loader the
 * early load used. A sleeve whose rows cannot be read back keeps its early rows and is named at
 * WARNING. The early books themselves are not changed: the hold and the execution diff keep them.
 */
inline SleeveBooks read_back_finalized_books(const SleeveBooks& loaded_before_finalize,
                                             const SleeveBookLoader& load_book) {
    SleeveBooks finalized;
    for (const auto& [strategy_name, early_rows] : loaded_before_finalize) {
        auto stored = load_book(strategy_name);
        if (stored.is_ok()) {
            finalized[strategy_name] = stored.value();
        } else {
            WARN("The finalized Day T-1 rows of " + strategy_name + " could not be read back (" +
                 std::string(stored.error()->what()) +
                 "): yesterday's table and CSV print the rows as loaded before the finalize, "
                 "whose realized P&L is the placeholder");
            finalized[strategy_name] = early_rows;
        }
    }
    return finalized;
}

}  // namespace trade_ngin
