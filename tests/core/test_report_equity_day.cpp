// The equity report's day banners: one test per row of the banner table, the texts
// asserted word for word.
#include <gtest/gtest.h>

#include "trade_ngin/core/report_equity_day.hpp"

using namespace trade_ngin;

namespace {
bool has(const std::string& s, const std::string& t) { return s.find(t) != std::string::npos; }
const char* kNext = "Updates will resume with the next trading session\xE2\x80\x99s data.";
}  // namespace

TEST(EquityDayBanner, OpenTodayAndYesterdayHasNothing) {
    const auto b = equity_day_banner("2026-06-23", false, "", "2026-06-22", false, "", "2026-06-22",
                                     true);
    EXPECT_TRUE(b.banner_html.empty());
    EXPECT_TRUE(b.red_box_text.empty());
    EXPECT_TRUE(b.yellow_note.empty());
    EXPECT_FALSE(b.hide_table);
}

TEST(EquityDayBanner, ClosedTodayForAHolidayShowsTheTableAndTheRedBox) {
    const auto b = equity_day_banner("2026-06-19", true, "Juneteenth", "2026-06-18", false, "",
                                     "2026-06-18", false);
    EXPECT_TRUE(has(b.banner_html, ">Today is Juneteenth</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "Equity markets are closed today (2026-06-19) due to a market holiday. No trades "
                    "were placed and positions are unchanged. Yesterday's results (2026-06-18) are "
                    "shown below."));
    EXPECT_TRUE(has(b.banner_html, "Please continue to monitor your positions closely."));
    EXPECT_EQ(b.red_box_text,
              std::string("<strong>No Executions:</strong> Equity markets are closed today, so no "
                          "executions were generated and portfolio holdings are unchanged. ") +
                  kNext + "\n");
    EXPECT_TRUE(b.red_box_with_banner);
    EXPECT_TRUE(b.yellow_note.empty());
    EXPECT_FALSE(b.hide_table);
}

TEST(EquityDayBanner, ClosedTodayOnASaturdayDoesNotSayHoliday) {
    // 2026-06-20 is a Saturday, yesterday (Friday) was open
    const auto b = equity_day_banner("2026-06-20", true, "", "2026-06-19", false, "", "2026-06-19",
                                     false);
    EXPECT_TRUE(has(b.banner_html, ">Today is Saturday</h2>"));
    EXPECT_TRUE(has(b.banner_html, "closed today (2026-06-20). No trades"));
    EXPECT_FALSE(has(b.banner_html, "market holiday"));
    EXPECT_FALSE(b.hide_table);
}

TEST(EquityDayBanner, ClosedYesterdayAndTodayHidesTheTable) {
    // Sunday 2026-06-21 after a Saturday
    const auto b = equity_day_banner("2026-06-21", true, "", "2026-06-20", true, "", "2026-06-18",
                                     false);
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Saturday</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "Equity markets were closed yesterday (2026-06-20) and are closed today, so no "
                    "new prices are available. No trades were placed and positions are unchanged. "
                    "The last trading session was 2026-06-18."));
    EXPECT_EQ(b.red_box_text,
              std::string("<strong>No Executions:</strong> Equity markets were closed yesterday, so "
                          "no executions were generated and portfolio holdings are unchanged. ") +
                  kNext + "\n");
    EXPECT_EQ(b.yellow_note,
              "<strong>Note:</strong> Yesterday's PnL data is not available.\n");
    EXPECT_TRUE(b.hide_table);
    EXPECT_FALSE(b.red_box_with_banner);
}

TEST(EquityDayBanner, ClosedYesterdayOpenTodayWithTradesPricesThemAtTheLastClose) {
    const auto b = equity_day_banner("2026-06-22", false, "", "2026-06-21", true, "", "2026-06-18",
                                     true);
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Sunday</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "Equity markets were closed yesterday (2026-06-21), so no new prices are "
                    "available. Today's trades are priced at the last close (2026-06-18)."));
    EXPECT_TRUE(b.red_box_text.empty());
    EXPECT_EQ(b.yellow_note,
              "<strong>Note:</strong> Yesterday's PnL data is not available.\n");
    EXPECT_TRUE(b.hide_table);
}

TEST(EquityDayBanner, ClosedYesterdayOpenTodayWithoutTradesDropsTheSentenceAndShowsTheRedBox) {
    // the Monday after a holiday weekend, nothing traded
    const auto b = equity_day_banner("2026-06-22", false, "", "2026-06-21", true, "", "2026-06-18",
                                     false);
    EXPECT_FALSE(has(b.banner_html, "Today's trades"));
    EXPECT_TRUE(has(b.banner_html, "closed yesterday (2026-06-21), so no new prices are available."));
    EXPECT_EQ(b.red_box_text,
              std::string("<strong>No Executions:</strong> Equity markets were closed yesterday, so "
                          "no executions were generated and portfolio holdings are unchanged. ") +
                  kNext + "\n");
    EXPECT_TRUE(b.hide_table);
}

TEST(EquityDayBanner, AHolidayYesterdayIsNamedByTheCalendar) {
    const auto b = equity_day_banner("2026-06-20", true, "", "2026-06-19", true, "Juneteenth",
                                     "2026-06-18", false);
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Juneteenth</h2>"));
}
