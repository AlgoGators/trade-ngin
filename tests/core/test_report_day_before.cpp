// tests/core/test_report_day_before.cpp
//
// The day banners, word for word. One test per row of the banner table: title, form, text,
// table shown or hidden.

#include <gtest/gtest.h>

#include <string>

#include "trade_ngin/core/report_day_before.hpp"

using namespace trade_ngin;

namespace {
bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}
}  // namespace

TEST(DayBefore, TheWeekdayOfADateIsRight) {
    EXPECT_EQ(report_weekday_of("2026-04-25"), 6);  // Saturday
    EXPECT_EQ(report_weekday_of("2026-04-26"), 0);  // Sunday
    EXPECT_EQ(report_weekday_of("2026-04-27"), 1);
    EXPECT_EQ(report_weekday_of("2026-01-19"), 1);  // MLK Day
}

TEST(DayBefore, ASaturdayWithNoPricesIsTheOriginalClosedForm) {
    const auto b = day_before_banner("2026-04-25", "", true, true);
    EXPECT_EQ(b.form, DayBeforeForm::Closed);
    EXPECT_TRUE(b.hide_table());
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Saturday</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "The latest futures settlement prices are not available, as futures markets were "
                    "closed yesterday (2026-04-25) due to it being a Saturday. The PnL for these "
                    "contracts will be updated in the next report once settlement data is released."));
    EXPECT_TRUE(has(b.banner_html, ">Please continue to monitor your positions closely.</p>"));
    EXPECT_EQ(b.red_box_text,
              "<strong>No Executions:</strong> Futures markets were closed yesterday, so no executions "
              "were generated and portfolio holdings are unchanged. Updates will resume with the next "
              "trading session\xE2\x80\x99s data.\n");
    EXPECT_EQ(b.yellow_note, "<strong>Note:</strong> Yesterday's PnL data is not available.\n");
}

TEST(DayBefore, ASaturdayWithSomePricesIsThePartialFormWithTheNewParagraph) {
    const auto b = day_before_banner("2026-06-20", "", false, true);
    EXPECT_EQ(b.form, DayBeforeForm::Partial);
    EXPECT_FALSE(b.hide_table());
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Saturday</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "Some futures settlement prices for 2026-06-20 are not available, as these contracts "
                    "did not trade. The PnL for these contracts will be updated in the next report once "
                    "settlement data is released."));
    EXPECT_TRUE(has(b.banner_html, ">Please monitor these positions closely.</p>"));
    EXPECT_TRUE(b.red_box_text.empty());
    EXPECT_TRUE(b.yellow_note.empty());
}

TEST(DayBefore, ASundayWithAgriculturalRowsMissingIsTheOriginalSundayBannerAndShowsTheTable) {
    const auto b = day_before_banner("2026-04-26", "", false, true);
    EXPECT_EQ(b.form, DayBeforeForm::Partial);
    EXPECT_FALSE(b.hide_table());
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Sunday</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "Agricultural futures settlement prices for Sunday (2026-04-26) are not yet "
                    "available, as these contracts begin trading Sunday evening. The PnL for these "
                    "contracts will be updated in the next report once settlement data is released."));
    EXPECT_TRUE(has(b.banner_html, ">Please monitor these positions closely.</p>"));
}

TEST(DayBefore, ASundayWithNothingMissingHasNoBanner) {
    const auto b = day_before_banner("2026-04-26", "", false, false);
    EXPECT_EQ(b.form, DayBeforeForm::None);
    EXPECT_TRUE(b.banner_html.empty());
}

TEST(DayBefore, AHolidayWithSomePricesMissingIsTheSundayFormWithTheHolidayTitle) {
    const auto b = day_before_banner("2026-01-19", "Martin Luther King Jr. Day", false, true);
    EXPECT_EQ(b.form, DayBeforeForm::Partial);
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Martin Luther King Jr. Day</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "Some futures settlement prices for 2026-01-19 are not available, as these contracts "
                    "did not trade. The PnL for these contracts will be updated in the next report once "
                    "settlement data is released."));
    EXPECT_TRUE(has(b.banner_html, ">Please monitor these positions closely.</p>"));
    EXPECT_FALSE(b.hide_table());
}

TEST(DayBefore, AHolidayWithNoPricesIsTheOriginalHolidayClosedForm) {
    const auto b = day_before_banner("2025-12-25", "Christmas Day", true, true);
    EXPECT_EQ(b.form, DayBeforeForm::Closed);
    EXPECT_TRUE(b.hide_table());
    EXPECT_TRUE(has(b.banner_html, ">Yesterday was Christmas Day</h2>"));
    EXPECT_TRUE(has(b.banner_html,
                    "The latest futures settlement prices are not available, as futures markets were "
                    "closed yesterday (2025-12-25) due to a federal holiday. The PnL for these contracts "
                    "will be updated in the next report once settlement data is released."));
    EXPECT_TRUE(has(b.banner_html, ">Please continue to monitor your positions closely.</p>"));
    EXPECT_EQ(b.red_box_text,
              "<strong>No Executions:</strong> Since yesterday (2025-12-25) was a market holiday, no "
              "new market data is available. Positions remain unchanged from the previous trading day, "
              "and no executions were generated.\n");
    EXPECT_EQ(b.yellow_note, "<strong>Note:</strong> Yesterday's PnL data is not available.\n");
}

TEST(DayBefore, AWeekdayWithNoPricesIsTheRedBoxAndTheYellowNoteWithNoBanner) {
    const auto b = day_before_banner("2026-04-22", "", true, true);
    EXPECT_EQ(b.form, DayBeforeForm::Closed);
    EXPECT_TRUE(b.banner_html.empty());
    EXPECT_EQ(b.red_box_text,
              "<strong>No Executions:</strong> No session was found for yesterday (2026-04-22), so no "
              "new market data is available. Positions remain unchanged from the previous trading day, "
              "and no executions were generated.\n");
    EXPECT_EQ(b.yellow_note, "<strong>Note:</strong> Yesterday's PnL data is not available.\n");
}

TEST(DayBefore, AWeekdayWithAFewRowsMissingAndANormalDayHaveNoBanner) {
    EXPECT_TRUE(day_before_banner("2026-04-22", "", false, true).banner_html.empty());
    EXPECT_EQ(day_before_banner("2026-04-22", "", false, true).form, DayBeforeForm::None);
    EXPECT_EQ(day_before_banner("2026-04-22", "", false, false).form, DayBeforeForm::None);
    // a holiday with nothing missing: no banner, whatever the calendar says
    EXPECT_EQ(day_before_banner("2026-01-19", "Martin Luther King Jr. Day", false, false).form,
              DayBeforeForm::None);
}
