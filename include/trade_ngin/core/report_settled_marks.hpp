// include/trade_ngin/core/report_settled_marks.hpp
//
// The two price columns of the settled day's table (the daily report's "Yesterday's Finalized
// Position Results" and the live file <T-1>_positions_asof_<T>.csv).
//
// The columns exist so a reader can work a row's P&L out in their head, the way the settle books
// it: quantity x (latest price - previous price) x contract size. So the two prices are the two
// the settle's own arithmetic used for the row (LivePnLManager::finalize_previous_day: its
// "T-2 close" and "T-1 close" maps), handed here by the runner. They are never looked up a second
// time from the bars.
//
//   Previous price   the price the day was settled FROM
//   Latest price     the price the day was settled AT
//
// A symbol that printed NO BAR on the settled day is one the settle found no price to settle at
// (it is not in the "settled at" map): the settle booked 0 for it. Its previous price is then its
// last known close, its latest price is the words "no bar", and one footnote under the table says
// what the words mean. The file leaves the cell empty.
//
// Header-only and pure: the three live runners, the report and the file writer call these, the
// tests pin them.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "trade_ngin/core/report_price_text.hpp"
#include "trade_ngin/data/finite_statement.hpp"

namespace trade_ngin {

/// The headers of the two columns in the report. The file's columns keep their names,
/// prior_mark and mark, and hold the same two prices.
inline constexpr const char* kSettledPreviousPriceLabel = "Previous price";
inline constexpr const char* kSettledLatestPriceLabel = "Latest price";
/// What the latest-price cell of the report prints for a symbol that printed no bar that day.
inline constexpr const char* kSettledNoBar = "\xE2\x80\x93";  // an en dash; the row carries footnote 1
/// The symbols whose settled bar booked no move, by the kind the settle knew:
/// a ROLL (the series switched contract on that bar: its two prices are of two contracts) and a
/// price NOT ACCEPTED (a withheld bar: the move is booked with the next price).
struct SettledZeroKinds {
    std::unordered_set<std::string> roll;
    std::unordered_set<std::string> withheld;
    /// With per_contract_rolls on, the symbols whose settled bar was a
    /// per-contract roll bar: the row books the old contract's move to its close on that bar (not
    /// $0.00) and carries the roll footnote (kSettledNoteRollBar). Empty with the switch off.
    std::unordered_set<std::string> per_contract_roll;
};
/// Footnote numbers of a settled row booked at zero (the report's footnote lines).
inline constexpr int kSettledNoteRoll = 2;
inline constexpr int kSettledNoteNotAccepted = 3;
/// The footnote of a settled row whose bar was a per-contract roll bar.
inline constexpr int kSettledNoteRollBar = 6;

/// kSettledNoteRollBar for a futures row with a quantity whose symbol's settled bar was a
/// per-contract roll bar, 0 otherwise (whatever its figure: the move is booked).
inline int settled_roll_bar_note(bool equities, const std::string& symbol, double quantity,
                                 const SettledZeroKinds& kinds) {
    return !equities && quantity != 0.0 && kinds.per_contract_roll.count(symbol) ? kSettledNoteRollBar : 0;
}

/// The footnote number of a futures row's $0.00: 2 (contract roll), 3 (price not accepted) or 0
/// (none). A row carries it when its symbol is in one of the two sets, the quantity is not zero,
/// the two prices are known and the figure prints as $0.00.
inline int settled_zero_note(bool equities, const std::string& symbol, double quantity,
                             std::optional<double> previous, std::optional<double> latest,
                             double realized, const SettledZeroKinds& kinds) {
    if (equities || quantity == 0.0 || !previous || !latest)
        return 0;
    if (report_dollars(realized) != "$0.00")
        return 0;
    if (kinds.withheld.count(symbol))
        return kSettledNoteNotAccepted;
    if (kinds.roll.count(symbol))
        return kSettledNoteRoll;
    return 0;
}

/// The two prices of every symbol of the settled table. A symbol absent from `latest` printed no
/// bar on the settled day; a symbol absent from `previous` has no known earlier price.
struct SettledTablePrices {
    std::unordered_map<std::string, double> previous;
    std::unordered_map<std::string, double> latest;
};

/**
 * @brief The settled table's two prices from the settle's own inputs.
 *
 * @param settled_from      the "from" map handed to finalize_previous_day (its T-2 closes)
 * @param settled_to        the "to" map handed to it (its T-1 closes); a symbol it does not hold
 *                          printed no bar and settled 0
 * @param last_known_close  for a symbol with no bar on the settled day, its last known close
 *
 * A symbol with a bar gets the pair the P&L was computed from. A symbol with no bar gets its last
 * known close as the previous price and no latest price: the "from" map's entry for it was never
 * used by the settle (it can be a bar older than the last one) and is not printed.
 */
inline SettledTablePrices settled_table_prices(
    const std::unordered_map<std::string, double>& settled_from,
    const std::unordered_map<std::string, double>& settled_to,
    const std::unordered_map<std::string, double>& last_known_close) {
    SettledTablePrices out;
    out.latest = settled_to;
    for (const auto& [symbol, price] : settled_from) {
        if (settled_to.count(symbol))
            out.previous[symbol] = price;
    }
    for (const auto& [symbol, price] : last_known_close) {
        if (!settled_to.count(symbol))
            out.previous[symbol] = price;
    }
    return out;
}

/// A symbol's price in one of the two maps, or nothing.
inline std::optional<double> settled_price_of(const std::unordered_map<std::string, double>& prices,
                                              const std::string& symbol) {
    const auto it = prices.find(symbol);
    if (it == prices.end())
        return std::nullopt;
    return it->second;
}

/// The two cells of one row of the report's table, and whether the row carries the words the
/// footnote explains.
struct SettledPriceCells {
    std::string previous;
    std::string latest;
    bool no_bar{false};
};

/**
 * @brief The report's two cells of a row. `latest` empty is the no-bar row: the cell reads
 *        "no bar" (no "n/a", no carried price) and the footnote prints. A previous price that is
 *        not known reads "n/a". A price prints at `places` decimals, with its dollar sign and
 *        thousands separators (report_price_dollars_places).
 */
inline SettledPriceCells settled_price_cells(std::optional<double> previous,
                                             std::optional<double> latest, int places = 4) {
    SettledPriceCells cells;
    cells.previous = previous ? report_price_dollars_places(*previous, places) : std::string("n/a");
    cells.no_bar = !latest.has_value();
    cells.latest = latest ? report_price_dollars_places(*latest, places) : std::string(kSettledNoBar);
    return cells;
}

/// The two cells of one row of the file: prior_mark and mark.
struct SettledCsvCells {
    std::string prior_mark;
    std::string mark;
};

/**
 * @brief The file's two cells of a row, from the same two prices as the report's: each the
 *        shortest text that reads back as the same number; an unknown price is an empty cell, so
 *        the mark of a no-bar row is empty (a 0 would read as a price) and its prior_mark is the
 *        last known close.
 */
inline SettledCsvCells settled_csv_cells(std::optional<double> previous,
                                         std::optional<double> latest) {
    SettledCsvCells cells;
    if (previous)
        cells.prior_mark = round_trip_number(*previous);
    if (latest)
        cells.mark = round_trip_number(*latest);
    return cells;
}

}  // namespace trade_ngin
