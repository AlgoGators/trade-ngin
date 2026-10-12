// include/trade_ngin/core/report_settled_marks.hpp
//
// The two price columns of the settled day's table (the daily report's "Yesterday's Finalized
// Position Results" and the live file <T-1>_positions_asof_<T>.csv), HD 2026-10-12.
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

#include "trade_ngin/core/report_price_text.hpp"
#include "trade_ngin/data/finite_statement.hpp"

namespace trade_ngin {

/// The headers of the two columns in the report. The file's columns keep their names,
/// prior_mark and mark, and hold the same two prices.
inline constexpr const char* kSettledPreviousPriceLabel = "Previous price";
inline constexpr const char* kSettledLatestPriceLabel = "Latest price";
/// What the latest-price cell of the report prints for a symbol that printed no bar that day.
inline constexpr const char* kSettledNoBar = "no bar";
/// The one footnote under the table, printed only when a row carries the words.
inline constexpr const char* kSettledNoBarFootnote =
    "no bar: the market did not print a price that day; the position's P&L for the day is 0";

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
 *        not known reads "n/a". A price prints every decimal it has (report_price_text).
 */
inline SettledPriceCells settled_price_cells(std::optional<double> previous,
                                             std::optional<double> latest) {
    SettledPriceCells cells;
    cells.previous = previous ? report_price_text(*previous, true) : std::string("n/a");
    cells.no_bar = !latest.has_value();
    cells.latest = latest ? report_price_text(*latest, true) : std::string(kSettledNoBar);
    return cells;
}

/// The footnote's line of the report, for a table with at least one no-bar row; empty otherwise.
/// The sentence is written into the page with its ampersand escaped ("P&amp;L").
inline std::string settled_no_bar_footnote_html(bool any_no_bar_row) {
    if (!any_no_bar_row)
        return {};
    std::string sentence;
    for (const char* c = kSettledNoBarFootnote; *c != '\0'; ++c) {
        if (*c == '&')
            sentence += "&amp;";
        else
            sentence += *c;
    }
    return "<div style=\"font-size: 12px; color: #666; margin: 4px 0 4px 0;\">" + sentence +
           "</div>\n";
}

/// The sentence under the settled table of a FUTURES book: what the two prices are and what a
/// reader can work out from a row. A futures row's Realized PnL is the settle's own figure,
/// quantity x (latest - previous) x contract size, so the row multiplies out. The settle books 0
/// by rule on two kinds of bar whatever the two closes are, and the sentence names them: a change
/// bar (the continuous series changes contract: the previous price is the outgoing contract's
/// close and the latest the incoming one's) and a withheld bar (a print the run did not accept).
/// A no-bar row is the footnote's.
inline constexpr const char* kSettledNoteFutures =
    "Previous price and Latest price are the two closes the settled day was booked between: "
    "quantity times the price change times the contract size is the row's Realized PnL, the "
    "settled day's stored figure. Two kinds of row are booked at $0.00 whatever their two prices "
    "show: a row on a day its price series changes to another contract (its two prices are then "
    "of two different contracts), and a row whose price for that day was not accepted (for "
    "example a bad print, or a contract change not yet confirmed).";

/// The sentence under the settled table of the EQUITY book. Its two prices are the pair the
/// settle measured the day's move between, closes of the series as the run loaded it: that
/// series is adjusted back for a dividend or a split whose ex-date bar it holds, and it ends at
/// the settled day, so the previous price is restated exactly when the settled day is the
/// ex-date (META 2026-06-15: 566.47888553 printed, 566.98 the close as it printed) and the
/// latest price never is. The table's Realized PnL cell is the row's realized P&L of that day,
/// not the day's move: the equity row stores the move in the value of its holding, which the
/// table has no column for. So the sentence says what quantity times the price change is, and
/// that it is not the cell beside it.
inline constexpr const char* kSettledNoteEquity =
    "Previous price and Latest price are the two closes the settled day's price move was "
    "measured between: quantity times the price change is that holding's gain or loss from the "
    "day's price move, which this table does not print. On a day a holding goes ex-dividend or "
    "splits, its previous price is the close before restated for that event, not the close as "
    "it printed. Realized PnL is the gain or loss the holding realized that day, the settled "
    "day's stored figure; it is not the day's price move, so a row is not expected to multiply "
    "out to it.";

/// The line under the settled table (and under its footnote, when there is one): the sentence of
/// the book's kind.
inline std::string settled_table_note_html(bool equities) {
    return std::string("<div style=\"font-size: 12px; color: #666; margin: 4px 0 12px 0;\">") +
           (equities ? kSettledNoteEquity : kSettledNoteFutures) + "</div>\n";
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
