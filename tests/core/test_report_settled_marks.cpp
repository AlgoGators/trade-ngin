// The two price columns of the settled day's table (core/report_settled_marks.hpp; HD 2026-10-12):
// "Previous price" is the price the day was settled FROM and "Latest price" the price it was
// settled AT, the pair the settle's own P&L arithmetic used, so on a row with a bar
//   quantity x (latest - previous) x contract size = the row's stored P&L of the day.
// A symbol that printed no bar settled 0: its previous price is its last known close, its latest
// price reads "no bar", and one footnote says what the words mean. The live settled file carries
// the same two prices in prior_mark and mark (an empty mark on a no-bar row).
//
// Every test here EXECUTES the rule with no database. The worked numbers are the CONSERVATIVE
// chain's own: KE.v.0 (1 contract, 50 a point; closes Thu 04-23 679.50, Fri 04-24 668.50, no
// Sunday session) and 6J.v.0 (-1 contract, 12,500,000 a point; closes 04-22 0.006297, 04-23
// 0.006286, no bar on 04-24).
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_map>

#include "trade_ngin/core/report_settled_marks.hpp"

using namespace trade_ngin;

namespace {

using PriceMap = std::unordered_map<std::string, double>;

double number_of(std::string text) {
    std::string digits;
    for (char c : text)
        if (c != ',')
            digits += c;
    return std::stod(digits);
}

// The cells of one symbol from the settle's three inputs.
SettledPriceCells cells_for(const std::string& symbol, const PriceMap& from, const PriceMap& to,
                            const PriceMap& last_known) {
    const SettledTablePrices prices = settled_table_prices(from, to, last_known);
    return settled_price_cells(settled_price_of(prices.previous, symbol),
                               settled_price_of(prices.latest, symbol));
}

SettledCsvCells csv_for(const std::string& symbol, const PriceMap& from, const PriceMap& to,
                        const PriceMap& last_known) {
    const SettledTablePrices prices = settled_table_prices(from, to, last_known);
    return settled_csv_cells(settled_price_of(prices.previous, symbol),
                             settled_price_of(prices.latest, symbol));
}

}  // namespace

// The labels are plain words: no "T-1", no "T-2", no "mark".
TEST(ReportSettledMarks, TheTwoColumnsAreNamedInPlainWords) {
    EXPECT_STREQ(kSettledPreviousPriceLabel, "Previous price");
    EXPECT_STREQ(kSettledLatestPriceLabel, "Latest price");
    for (const std::string label : {kSettledPreviousPriceLabel, kSettledLatestPriceLabel}) {
        EXPECT_EQ(label.find("T-1"), std::string::npos) << label;
        EXPECT_EQ(label.find("T-2"), std::string::npos) << label;
    }
}

// An ordinary row: the email of Sat 04-25 settles Fri 04-24. KE was settled from Thursday's
// 679.50 at Friday's 668.50: 1 x (668.50 - 679.50) x 50 = -550.00, the stored P&L.
TEST(ReportSettledMarks, AnOrdinaryRowPrintsThePairItsPnlWasBookedOn) {
    const PriceMap from{{"KE.v.0", 679.5}};
    const PriceMap to{{"KE.v.0", 668.5}};
    const SettledPriceCells cells = cells_for("KE.v.0", from, to, {});
    EXPECT_EQ(cells.previous, "679.50");
    EXPECT_EQ(cells.latest, "668.50");
    EXPECT_FALSE(cells.no_bar);
    EXPECT_DOUBLE_EQ(1.0 * (number_of(cells.latest) - number_of(cells.previous)) * 50.0, -550.0);
    // a last known close is not what a row with a bar prints
    EXPECT_EQ(cells_for("KE.v.0", from, to, {{"KE.v.0", 1.0}}).previous, "679.50");
}

// A no-bar row: the email of Mon 04-27 settles Sun 04-26, and KE has no Sunday session. The settle
// found no price to settle at and booked 0.00. The "from" map still holds Thursday's 679.50 for KE
// (the bar before its newest bar: the old lookup, never used by the settle). The row prints the
// last known close, Friday's 668.50, and the words "no bar".
TEST(ReportSettledMarks, ANoBarRowPrintsItsLastKnownCloseAndTheWordsNoBar) {
    const PriceMap from{{"KE.v.0", 679.5}, {"ZN.v.0", 110.859375}};
    const PriceMap to{{"ZN.v.0", 110.890625}};
    const PriceMap last_known{{"KE.v.0", 668.5}};
    const SettledPriceCells cells = cells_for("KE.v.0", from, to, last_known);
    EXPECT_EQ(cells.previous, "668.50") << "Friday's close, the last known price";
    EXPECT_NE(cells.previous, "679.50") << "Thursday's close was settled on Friday";
    EXPECT_EQ(cells.latest, "no bar");
    EXPECT_TRUE(cells.no_bar);
    EXPECT_EQ(cells.latest.find("n/a"), std::string::npos);
    EXPECT_EQ(cells.previous.find("n/a"), std::string::npos);
    // the row beside it, which printed a bar, is unchanged by the other row's missing bar
    const SettledPriceCells zn = cells_for("ZN.v.0", from, to, last_known);
    EXPECT_EQ(zn.previous, "110.859375");
    EXPECT_EQ(zn.latest, "110.890625");
    EXPECT_FALSE(zn.no_bar);
}

// A feed hole: 6J printed no bar on Fri 04-24 (the email of Sat 04-25). Its "from" entry is the
// 04-22 close 0.006297; its last known close is the 04-23 bar, 0.006286.
TEST(ReportSettledMarks, AFeedHoleRowPrintsItsLastKnownCloseNotTheBarBeforeIt) {
    const PriceMap from{{"6J.v.0", 0.006297}};
    const PriceMap last_known{{"6J.v.0", 0.006286}};
    const SettledPriceCells cells = cells_for("6J.v.0", from, {}, last_known);
    EXPECT_EQ(cells.previous, "0.006286");
    EXPECT_EQ(cells.latest, "no bar");
    EXPECT_TRUE(cells.no_bar);
}

// A symbol with no bar that day and no earlier price at all: nothing is known of it.
TEST(ReportSettledMarks, ARowWithNoKnownPriceSaysSo) {
    const SettledPriceCells cells = cells_for("NEW.v.0", {{"NEW.v.0", 12.5}}, {}, {});
    EXPECT_EQ(cells.previous, "n/a") << "an unused from-price is not printed as the last close";
    EXPECT_EQ(cells.latest, "no bar");
    EXPECT_TRUE(cells.no_bar);
    // a bar with no earlier price: the settle booked 0 for want of a price to settle from
    const SettledPriceCells first = cells_for("NEW.v.0", {}, {{"NEW.v.0", 12.5}}, {});
    EXPECT_EQ(first.previous, "n/a");
    EXPECT_EQ(first.latest, "12.50");
    EXPECT_FALSE(first.no_bar);
}

// The footnote: the exact sentence, once, and only under a table with a no-bar row.
TEST(ReportSettledMarks, TheFootnotePrintsOnlyWhenARowHasNoBar) {
    EXPECT_STREQ(kSettledNoBarFootnote,
                 "no bar: the market did not print a price that day; the position's P&L for the "
                 "day is 0");
    EXPECT_EQ(settled_no_bar_footnote_html(false), "");
    const std::string line = settled_no_bar_footnote_html(true);
    EXPECT_NE(line.find(">no bar: the market did not print a price that day; the position's "
                        "P&amp;L for the day is 0</div>\n"),
              std::string::npos)
        << line;
    // the flag of a table is the "or" of its rows' flags
    const PriceMap from{{"KE.v.0", 679.5}};
    const PriceMap to{{"KE.v.0", 668.5}};
    EXPECT_EQ(settled_no_bar_footnote_html(cells_for("KE.v.0", from, to, {}).no_bar), "");
    EXPECT_NE(
        settled_no_bar_footnote_html(cells_for("KE.v.0", from, {}, {{"KE.v.0", 668.5}}).no_bar),
        "");
}

// The sentence under the table (the lead's audit of 2026-10-12, B-3): it said "The two marks are
// raw closes ... so a row need not multiply out" on every book. Since the two columns are the
// pair the settle booked the row on, a futures row DOES multiply out to the cell beside it; the
// sentence says so and names the two kinds of row the settle books at 0 whatever their prices.
TEST(ReportSettledMarks, TheFuturesSentenceSaysARowMultipliesOutAndNamesTheRowsThatDoNot) {
    EXPECT_STREQ(
        kSettledNoteFutures,
        "Previous price and Latest price are the two closes the settled day was booked between: "
        "quantity times the price change times the contract size is the row's Realized PnL, the "
        "settled day's stored figure. Two kinds of row are booked at $0.00 whatever their two "
        "prices show: a row on a day its price series changes to another contract (its two prices "
        "are then of two different contracts), and a row whose price for that day was not accepted "
        "(for example a bad print, or a contract change not yet confirmed).");
    EXPECT_EQ(settled_table_note_html(false),
              std::string("<div style=\"font-size: 12px; color: #666; margin: 4px 0 12px 0;\">") +
                  kSettledNoteFutures + "</div>\n");
    // the worked row the sentence speaks of, from the printed cells: KE Friday, 1 contract, 50 a
    // point, booked -550.00
    const SettledPriceCells ke = cells_for("KE.v.0", {{"KE.v.0", 679.5}}, {{"KE.v.0", 668.5}}, {});
    EXPECT_DOUBLE_EQ(1.0 * (number_of(ke.latest) - number_of(ke.previous)) * 50.0, -550.0);
}

// The equity sentence. The equity table has no column for the day's price move (its Realized PnL
// cell is the row's realized P&L of the day), and on an ex-dividend settle the previous price is
// the close before restated for the dividend. The worked row is the chain's own: META, settled
// 2026-06-15 (ex-dividend, 0.525 a share), 7.147202 shares, the close before 566.98 as printed
// and 566.4788855313 as the settle used it, the close of the day 593.48.
TEST(ReportSettledMarks, TheEquitySentenceSaysWhatTheTwoPricesAreAndThatTheCellBesideIsNotTheMove) {
    EXPECT_STREQ(
        kSettledNoteEquity,
        "Previous price and Latest price are the two closes the settled day's price move was "
        "measured between: quantity times the price change is that holding's gain or loss from "
        "the day's price move, which this table does not print. On a day a holding goes "
        "ex-dividend or splits, its previous price is the close before restated for that event, "
        "not the close as it printed. Realized PnL is the gain or loss the holding realized that "
        "day, the settled day's stored figure; it is not the day's price move, so a row is not "
        "expected to multiply out to it.");
    EXPECT_EQ(settled_table_note_html(true),
              std::string("<div style=\"font-size: 12px; color: #666; margin: 4px 0 12px 0;\">") +
                  kSettledNoteEquity + "</div>\n");

    const SettledPriceCells meta =
        cells_for("META", {{"META", 566.4788855313}}, {{"META", 593.48}}, {});
    EXPECT_EQ(meta.previous, "566.47888553");  // the restated close is what prints, not 566.98
    EXPECT_EQ(meta.latest, "593.48");
    // quantity x the price change, from the printed cells, is the move the settle computed
    // (192.982419 in its own log line); with the close as it printed it would be short by the
    // dividend
    const double shares = 7.147202;
    EXPECT_NEAR(shares * (number_of(meta.latest) - number_of(meta.previous)), 192.982419, 1e-4);
    EXPECT_NEAR(shares * (593.48 - 566.98), 189.400853, 1e-4);
}

// Neither sentence carries the old claims or a notation a reader has to decode.
TEST(ReportSettledMarks, NeitherSentenceSaysRawClosesOrThatARowNeedNotMultiplyOut) {
    for (const bool equities : {false, true}) {
        const std::string line = settled_table_note_html(equities);
        for (const char* gone :
             {"raw close", "two marks", "need not multiply", "recomputed", "T-1", "T-2", "mark",
              "Mark", "adjusted", "settle_", "junk", "withheld", "change bar"}) {
            EXPECT_EQ(line.find(gone), std::string::npos) << gone << " in: " << line;
        }
        EXPECT_NE(line.find("Previous price and Latest price"), std::string::npos);
    }
    EXPECT_NE(settled_table_note_html(false), settled_table_note_html(true));
}

// The file's two cells for the same three inputs: the same prices as numbers, an empty mark on a
// no-bar row, and prior_mark the last known close there.
TEST(ReportSettledMarks, TheFilesTwoCellsCarryTheSamePrices) {
    const SettledCsvCells ordinary =
        csv_for("KE.v.0", {{"KE.v.0", 679.5}}, {{"KE.v.0", 668.5}}, {});
    EXPECT_EQ(ordinary.prior_mark, "679.5");
    EXPECT_EQ(ordinary.mark, "668.5");
    EXPECT_DOUBLE_EQ(1.0 * (std::stod(ordinary.mark) - std::stod(ordinary.prior_mark)) * 50.0,
                     -550.0);

    const SettledCsvCells no_bar = csv_for("KE.v.0", {{"KE.v.0", 679.5}}, {}, {{"KE.v.0", 668.5}});
    EXPECT_EQ(no_bar.prior_mark, "668.5");
    EXPECT_EQ(no_bar.mark, "") << "the mark of a no-bar row stays empty";

    const SettledCsvCells hole =
        csv_for("6J.v.0", {{"6J.v.0", 0.006297}}, {}, {{"6J.v.0", 0.006286}});
    EXPECT_EQ(hole.prior_mark, "0.006286");
    EXPECT_EQ(hole.mark, "");

    const SettledCsvCells unknown = csv_for("NEW.v.0", {}, {}, {});
    EXPECT_EQ(unknown.prior_mark, "");
    EXPECT_EQ(unknown.mark, "");
    // a 6J price keeps every digit in both forms
    EXPECT_EQ(settled_csv_cells(0.0062995, 0.0062925).prior_mark, "0.0062995");
    EXPECT_EQ(settled_price_cells(0.0062995, 0.0062925).previous, "0.0062995");
}
