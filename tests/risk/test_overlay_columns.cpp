// tests/risk/test_overlay_columns.cpp
//
// The six overlay columns of trading.live_results (migration 030; HD 2026-10-10; the lead's ten
// rulings): which cells have a value, the one-day VaR's formula, the signed net leverage, and
// where the runners take them from. The readings themselves are the overlay's
// (tests/risk/test_overlay.cpp) and the accessor's equality with the pass is
// OnePassBookTest.TheOverlaysReadingsOfTheStoredBookAreThePassesToTheBit.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trade_ngin/live/live_statistics_columns.hpp"
#include "trade_ngin/risk/overlay_columns.hpp"

using namespace trade_ngin;

namespace {

overlay::Readings readings(bool covariance, double risk, double jump, double shock, double gross,
                           double net) {
    overlay::Readings r;
    r.covariance_readings = covariance;
    r.risk = risk;
    r.jump = jump;
    r.shock = shock;
    r.gross = gross;
    r.net = net;
    return r;
}

std::string read_source(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) {
            std::ifstream in(dir / relative);
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
        dir = dir.parent_path();
    }
    return {};
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

}  // namespace

TEST(OverlayColumns, TheSixAreTheReadingsAndTheVarIsOnePointSixFourFiveDailySigmasInDollars) {
    // R 0.10 a year on a window of 256 bars a year and a sizing capital of 500,000:
    // 1.645 x 0.10 / sqrt(256) x 500,000 = 1.645 x 0.00625 x 500,000 = 5,140.625 dollars.
    const OverlayColumns c =
        overlay_columns_of(true, readings(true, 0.10, 0.25, 0.30, 1.6, 0.4), 256.0, 500000.0);
    ASSERT_TRUE(c.overlay_risk && c.overlay_risk_jump && c.overlay_risk_shock &&
                c.overlay_gross_leverage && c.overlay_net_leverage && c.var_95_1d);
    EXPECT_EQ(*c.overlay_risk, 0.10);
    EXPECT_EQ(*c.overlay_risk_jump, 0.25);
    EXPECT_EQ(*c.overlay_risk_shock, 0.30);
    EXPECT_EQ(*c.overlay_gross_leverage, 1.6);
    EXPECT_EQ(*c.overlay_net_leverage, 0.4);
    EXPECT_NEAR(*c.var_95_1d, 5140.625, 1e-9);
    // The window's own factor: on 311.0574 bars a year the same R is a smaller daily sigma.
    const OverlayColumns grid =
        overlay_columns_of(true, readings(true, 0.10, 0.25, 0.30, 1.6, 0.4), 311.0574, 495186.141684);
    EXPECT_NEAR(*grid.var_95_1d, 1.645 * 0.10 / std::sqrt(311.0574) * 495186.141684, 1e-9);
    EXPECT_NEAR(*grid.var_95_1d, 4618.640, 1e-3);
}

TEST(OverlayColumns, TheNetLeverageIsStoredSigned) {
    const OverlayColumns c =
        overlay_columns_of(true, readings(true, 0.08, 0.2, 0.2, 1.5, -0.45), 250.0, 500000.0);
    ASSERT_TRUE(c.overlay_net_leverage.has_value());
    EXPECT_EQ(*c.overlay_net_leverage, -0.45);
    ASSERT_TRUE(c.var_95_1d.has_value());
    EXPECT_GT(*c.var_95_1d, 0.0) << "the VaR is positive whatever the book's side";
}

TEST(OverlayColumns, ABlindWindowStoresOnlyTheTwoLeverageReadings) {
    // The overlay's code leaves 0.0 in the three risk readings of a blind window: never stored.
    const OverlayColumns c =
        overlay_columns_of(true, readings(false, 0.0, 0.0, 0.0, 1.2, 0.3), 0.0, 500000.0);
    EXPECT_FALSE(c.overlay_risk.has_value());
    EXPECT_FALSE(c.overlay_risk_jump.has_value());
    EXPECT_FALSE(c.overlay_risk_shock.has_value());
    EXPECT_FALSE(c.var_95_1d.has_value());
    ASSERT_TRUE(c.overlay_gross_leverage.has_value());
    EXPECT_EQ(*c.overlay_gross_leverage, 1.2);
    ASSERT_TRUE(c.overlay_net_leverage.has_value());
    EXPECT_EQ(*c.overlay_net_leverage, 0.3);
    // A reading with no annualisation factor has no VaR (the division is guarded).
    const OverlayColumns no_factor =
        overlay_columns_of(true, readings(true, 0.1, 0.2, 0.2, 1.2, 0.3), 0.0, 500000.0);
    EXPECT_TRUE(no_factor.overlay_risk.has_value());
    EXPECT_FALSE(no_factor.var_95_1d.has_value());
}

TEST(OverlayColumns, ARowWithNoRiskDetailHasNoneOfTheSix) {
    const OverlayColumns c =
        overlay_columns_of(false, readings(true, 0.1, 0.2, 0.3, 1.2, 0.3), 256.0, 500000.0);
    EXPECT_FALSE(c.overlay_risk || c.overlay_risk_jump || c.overlay_risk_shock ||
                 c.overlay_gross_leverage || c.overlay_net_leverage || c.var_95_1d);
    const auto cells = overlay_cells(c, live_results_number);
    ASSERT_EQ(cells.size(), 6u);
    for (const auto& cell : cells) EXPECT_FALSE(cell.value.has_value()) << cell.column;
}

TEST(OverlayColumns, TheCellsAreTheSixNumericColumnsInTheMigrationsOrder) {
    const OverlayColumns c = overlay_columns_of(
        true, readings(true, 0.079083057224812459, 0.2213973284855956, 0.18670196368902256,
                       1.602357039519787, -0.41184512819048769),
        258.25, 495186.141684);
    const auto cells = overlay_cells(c, live_results_number);
    const std::vector<std::string> names = {"overlay_risk", "overlay_risk_jump",
                                            "overlay_risk_shock", "overlay_gross_leverage",
                                            "overlay_net_leverage", "var_95_1d"};
    ASSERT_EQ(cells.size(), names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        EXPECT_EQ(cells[i].column, names[i]);
        EXPECT_EQ(cells[i].type, "numeric");
        ASSERT_TRUE(cells[i].value.has_value());
    }
    EXPECT_EQ(*cells[0].value, "0.07908305722");  // ten significant digits
    EXPECT_EQ(*cells[4].value, "-0.4118451282");
    // None of the six is a statistic of the Day T-1 refresh.
    for (const auto& cell : live_statistics_cells(LiveStatisticsColumns{})) {
        EXPECT_EQ(cell.column.find("overlay_"), std::string::npos) << cell.column;
        EXPECT_NE(cell.column, "var_95_1d");
    }
}

TEST(OverlayColumns, TheLogLineSaysWhetherTheStoredBooksReadingsAreThePasss) {
    OnePassDay day;
    day.ran = day.sized = true;
    day.covariance_readings = true;
    day.overlay_risk = 0.10;
    day.overlay_risk_jump = 0.25;
    day.overlay_risk_shock = 0.30;
    day.overlay_gross_leverage = 1.6;
    day.overlay_net_leverage = 0.4;
    day.window_bars_per_year = 256.0;
    day.sizing_capital = 500000.0;
    StoredBookReadings stored;
    stored.readings = readings(true, 0.10, 0.25, 0.30, 1.6, 0.4);
    stored.contracts_held = 15;
    stored.contracts_in_risk = 14;
    const OverlayColumns c = overlay_columns_of(true, stored.readings, 256.0, 500000.0);
    const std::string same = overlay_stored_line("2026-04-24", day, stored, c, 0);
    EXPECT_NE(same.find("OVERLAY_STORED date=2026-04-24 covariance=1 R=0.10000000000000001"),
              std::string::npos)
        << same;
    EXPECT_NE(same.find(" bars_per_year=256 capital=500000 var_95_1d=5140.625 in_risk=14 held=15 "),
              std::string::npos)
        << same;
    EXPECT_NE(same.find(" equal_to_pass=1 rolled_back=0"), std::string::npos) << same;
    stored.readings.gross = 1.5;
    EXPECT_NE(overlay_stored_line("2026-04-24", day, stored, c, 1).find(" equal_to_pass=0 rolled_back=1"),
              std::string::npos);
}

// The runners are `main()`s: what is tested is the structure in the source.
TEST(OverlayColumns, TheFuturesRunnersReadTheBookTheyStoreThroughThePortfolioManager) {
    const std::string cons = read_source("apps/strategies/live_portfolio_conservative.cpp");
    const std::string base = read_source("apps/strategies/live_portfolio.cpp");
    const std::string equity = read_source("apps/strategies/live_equity_mean_reversion.cpp");
    if (cons.empty() || base.empty() || equity.empty()) {
        GTEST_SKIP() << "runner sources not found from the test working directory";
    }
    for (const std::string* src : {&cons, &base}) {
        // One reading, of the stored book, only on a rebalance the overlay answered.
        EXPECT_EQ(count_of(*src, "portfolio->overlay_readings_for_book("), 1u);
        EXPECT_NE(src->find("overlay_answered ? portfolio->overlay_readings_for_book(\n"
                            "                                   trade_ngin::account_book_of(strategy_positions_map))"),
                  std::string::npos);
        EXPECT_NE(src->find("const bool overlay_answered = one_pass_day.stores_detail();"),
                  std::string::npos);
        EXPECT_EQ(count_of(*src, "trade_ngin::overlay_columns_of("), 1u);
        // After the STRICT step that can roll a row back, before the row is written.
        const auto strict = src->find("std::vector<std::string> strict_rolled_back;");
        const auto read = src->find("portfolio->overlay_readings_for_book(");
        const auto written = src->find("trade_ngin::overlay_cells(overlay_columns, live_results_number)");
        ASSERT_NE(strict, std::string::npos);
        ASSERT_NE(written, std::string::npos);
        EXPECT_LT(strict, read);
        EXPECT_LT(read, written);
        // The row's own cells: the Day T-1 refresh takes the statistics cells alone.
        EXPECT_EQ(count_of(*src, "overlay_cells("), 1u);
        EXPECT_NE(src->find("previous_date, metric_updates, &refreshed_rows,\n"
                            "                        settled_statistics_cells);"),
                  std::string::npos);
    }
    // No overlay on the equity book: its rows leave the six NULL.
    EXPECT_EQ(equity.find("overlay_readings_for_book"), std::string::npos);
    EXPECT_EQ(equity.find("overlay_cells("), std::string::npos);
    // The loop's record is untouched: risk_detail_json reads none of the new fields.
    const std::string detail = read_source("include/trade_ngin/risk/risk_detail.hpp");
    const auto json = detail.find("inline nlohmann::json risk_detail_json(");
    ASSERT_NE(json, std::string::npos);
    EXPECT_EQ(detail.find("overlay_risk", json), std::string::npos);
    EXPECT_EQ(detail.find("window_bars_per_year", json), std::string::npos);
}
