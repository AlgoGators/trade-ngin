// The two price columns of the settled day's table (core/report_settled_marks.hpp):
// "Previous price" is the price the day was settled FROM and "Latest price" the price it was
// settled AT, the pair the settle's own P&L arithmetic used, so on a row with a bar
//   quantity x (latest - previous) x contract size = the row's stored P&L of the day.
// A symbol that printed no bar settled 0: its previous price is its last known close, its latest
// price reads "\xE2\x80\x93", and one footnote says what the words mean. The live settled file carries
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
        if (c != ',' && c != '$')
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
    EXPECT_EQ(cells.previous, "$679.5000");
    EXPECT_EQ(cells.latest, "$668.5000");
    EXPECT_FALSE(cells.no_bar);
    EXPECT_DOUBLE_EQ(1.0 * (number_of(cells.latest) - number_of(cells.previous)) * 50.0, -550.0);
    // a last known close is not what a row with a bar prints
    EXPECT_EQ(cells_for("KE.v.0", from, to, {{"KE.v.0", 1.0}}).previous, "$679.5000");
}

// A no-bar row: the email of Mon 04-27 settles Sun 04-26, and KE has no Sunday session. The settle
// found no price to settle at and booked 0.00. The "from" map still holds Thursday's 679.50 for KE
// (the bar before its newest bar: the old lookup, never used by the settle). The row prints the
// last known close, Friday's 668.50, and the words "\xE2\x80\x93".
TEST(ReportSettledMarks, ANoBarRowPrintsItsLastKnownCloseAndTheWordsNoBar) {
    const PriceMap from{{"KE.v.0", 679.5}, {"ZN.v.0", 110.859375}};
    const PriceMap to{{"ZN.v.0", 110.890625}};
    const PriceMap last_known{{"KE.v.0", 668.5}};
    const SettledPriceCells cells = cells_for("KE.v.0", from, to, last_known);
    EXPECT_EQ(cells.previous, "$668.5000") << "Friday's close, the last known price";
    EXPECT_NE(cells.previous, "$679.5000") << "Thursday's close was settled on Friday";
    EXPECT_EQ(cells.latest, "\xE2\x80\x93");
    EXPECT_TRUE(cells.no_bar);
    EXPECT_EQ(cells.latest.find("n/a"), std::string::npos);
    EXPECT_EQ(cells.previous.find("n/a"), std::string::npos);
    // the row beside it, which printed a bar, is unchanged by the other row's missing bar
    const SettledPriceCells zn = cells_for("ZN.v.0", from, to, last_known);
    EXPECT_EQ(zn.previous, "$110.8594");
    EXPECT_EQ(zn.latest, "$110.8906");
    EXPECT_FALSE(zn.no_bar);
}

// A feed hole: 6J printed no bar on Fri 04-24 (the email of Sat 04-25). Its "from" entry is the
// 04-22 close 0.006297; its last known close is the 04-23 bar, 0.006286.
TEST(ReportSettledMarks, AFeedHoleRowPrintsItsLastKnownCloseNotTheBarBeforeIt) {
    const PriceMap from{{"6J.v.0", 0.006297}};
    const PriceMap last_known{{"6J.v.0", 0.006286}};
    const SettledPriceCells cells = cells_for("6J.v.0", from, {}, last_known);
    EXPECT_EQ(cells.previous, "$0.0063");
    EXPECT_EQ(cells.latest, "\xE2\x80\x93");
    EXPECT_TRUE(cells.no_bar);
}

// A symbol with no bar that day and no earlier price at all: nothing is known of it.
TEST(ReportSettledMarks, ARowWithNoKnownPriceSaysSo) {
    const SettledPriceCells cells = cells_for("NEW.v.0", {{"NEW.v.0", 12.5}}, {}, {});
    EXPECT_EQ(cells.previous, "n/a") << "an unused from-price is not printed as the last close";
    EXPECT_EQ(cells.latest, "\xE2\x80\x93");
    EXPECT_TRUE(cells.no_bar);
    // a bar with no earlier price: the settle booked 0 for want of a price to settle from
    const SettledPriceCells first = cells_for("NEW.v.0", {}, {{"NEW.v.0", 12.5}}, {});
    EXPECT_EQ(first.previous, "n/a");
    EXPECT_EQ(first.latest, "$12.5000");
    EXPECT_FALSE(first.no_bar);
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
    EXPECT_EQ(settled_price_cells(0.0062995, 0.0062925, 7).previous, "$0.0062995");
}

// A futures row the settle booked at zero carries a footnote number for its kind.
TEST(ReportSettledMarks, AZeroRowCarriesTheNumberOfItsKindAndNoOtherRowIsNumbered) {
    SettledZeroKinds kinds;
    kinds.roll.insert("MBT.v.0");
    kinds.withheld.insert("6L.v.0");
    EXPECT_EQ(settled_zero_note(false, "MBT.v.0", 1.0, 79070.0, 77740.0, 0.0, kinds), 2);
    EXPECT_EQ(settled_zero_note(false, "6L.v.0", 1.0, 0.2, 0.19, 0.0, kinds), 3);
    EXPECT_EQ(settled_zero_note(false, "ES.v.0", 1.0, 1.0, 2.0, 0.0, kinds), 0);
    EXPECT_EQ(settled_zero_note(false, "MBT.v.0", 1.0, 1.0, 2.0, 12.5, kinds), 0);
    EXPECT_EQ(settled_zero_note(true, "MBT.v.0", 1.0, 1.0, 2.0, 0.0, kinds), 0);
    EXPECT_EQ(settled_zero_note(false, "MBT.v.0", 1.0, 1.0, std::nullopt, 0.0, kinds), 0);
    EXPECT_EQ(settled_zero_note(false, "MBT.v.0", 0.0, 1.0, 2.0, 0.0, kinds), 0);
    kinds.roll.insert("6L.v.0");
    EXPECT_EQ(settled_zero_note(false, "6L.v.0", 1.0, 0.2, 0.19, 0.0, kinds), 3);
}

// A price prints four decimals, or its tick's decimals when four cannot show a tick.
TEST(ReportSettledMarks, APriceHasFourDecimalsOrItsTicksAndNoBarIsADash) {
    EXPECT_EQ(report_price_places(0.25), 4);
    EXPECT_EQ(report_price_places(0.0000005), 7);
    EXPECT_EQ(report_price_places(0.015625), 6);
    EXPECT_EQ(report_price_dollars_places(668.5, 4), "$668.5000");
    EXPECT_EQ(report_price_dollars_places(79070.0, 4), "$79,070.0000");
    EXPECT_EQ(report_price_dollars_places(0.006286, 7), "$0.0062860");
    const SettledPriceCells cells = settled_price_cells(679.5, std::nullopt);
    EXPECT_EQ(cells.latest, "\xE2\x80\x93");
    EXPECT_TRUE(cells.no_bar);
}
