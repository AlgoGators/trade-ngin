// tests/backtest/test_csv_bar_source.cpp
//
// The backtest-only CSV bar source must produce EXACTLY what the equities_data.ohlcv_1d
// path produces for the same primitives: raw OHLC scaled by the backward factor from
// market_data_utils::compute_backward_adjustment_factors, anchored at 1 on the last bar
// of the requested window, volume raw, timestamps at UTC midnight. The fixture is the
// checked-in tests/fixtures/csv_bars/SPLITDIV.csv (a $0.50 dividend on row 3 and a 2:1
// split on row 6), reproduced inline so the test does not depend on the working directory.
// Prices are Decimal (8 fractional digits) on every path, so comparisons use 1e-7.
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/csv_bar_source.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/data/market_data_utils.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

const char* kSplitDiv =
    "date,open,high,low,close,volume,div_cash,split_factor\n"
    "2024-01-02,100.000000,101.000000,99.000000,100.000000,1000,0.000000,1.000000\n"
    "2024-01-03,100.000000,102.000000,99.000000,101.000000,1000,0.000000,1.000000\n"
    "2024-01-04,101.000000,103.000000,100.000000,102.000000,1000,0.500000,1.000000\n"
    "2024-01-05,102.000000,104.000000,101.000000,103.000000,1000,0.000000,1.000000\n"
    "2024-01-08,103.000000,105.000000,102.000000,104.000000,1000,0.000000,1.000000\n"
    "2024-01-09,52.000000,53.000000,51.000000,52.000000,2000,0.000000,2.000000\n"
    "2024-01-10,52.000000,54.000000,51.000000,53.000000,2000,0.000000,1.000000\n"
    "2024-01-11,53.000000,55.000000,52.000000,54.000000,2000,0.000000,1.000000\n";

Timestamp utc(int y, int m, int d) {
    Timestamp out;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    EXPECT_TRUE(core::parse_utc_date(buf, out));
    return out;
}

class CsvBarSourceTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        dir_ = std::filesystem::temp_directory_path() /
               ("csv_bar_source_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
        write("SPLITDIV", kSplitDiv);
    }
    void TearDown() override {
        std::filesystem::remove_all(dir_);
        TestBase::TearDown();
    }
    void write(const std::string& symbol, const std::string& content) {
        std::ofstream out(csv_bar_path(dir_, symbol));
        out << content;
    }
    std::filesystem::path dir_;
};

}  // namespace

TEST_F(CsvBarSourceTest, ReadsEveryRowRawAndUnadjusted) {
    auto rows = read_csv_bar_file(csv_bar_path(dir_, "SPLITDIV"));
    ASSERT_TRUE(rows.is_ok()) << rows.error()->what();
    ASSERT_EQ(rows.value().size(), 8u);
    EXPECT_DOUBLE_EQ(rows.value()[2].div_cash, 0.5);
    EXPECT_DOUBLE_EQ(rows.value()[5].split_factor, 2.0);
    EXPECT_DOUBLE_EQ(rows.value()[5].close, 52.0);  // raw, not adjusted
    EXPECT_EQ(rows.value()[0].time, utc(2024, 1, 2));
}

TEST_F(CsvBarSourceTest, SplitAndDividendMatchComputeBackwardAdjustmentFactors) {
    auto rows = read_csv_bar_file(csv_bar_path(dir_, "SPLITDIV")).value();
    std::vector<market_data_utils::AdjustmentBar> prim;
    for (const auto& r : rows) prim.push_back({r.close, r.div_cash, r.split_factor});
    auto factors = market_data_utils::compute_backward_adjustment_factors(prim);

    auto bars = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 2), utc(2024, 1, 11));
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    ASSERT_EQ(bars.value().size(), 8u);
    for (size_t i = 0; i < 8; ++i) {
        const auto& b = bars.value()[i];
        EXPECT_NEAR(b.close.as_double(), rows[i].close * factors[i], 1e-7) << "row " << i;
        EXPECT_NEAR(b.open.as_double(), rows[i].open * factors[i], 1e-7) << "row " << i;
        EXPECT_NEAR(b.high.as_double(), rows[i].high * factors[i], 1e-7) << "row " << i;
        EXPECT_NEAR(b.low.as_double(), rows[i].low * factors[i], 1e-7) << "row " << i;
        EXPECT_DOUBLE_EQ(b.volume, rows[i].volume) << "volume stays raw, row " << i;
        EXPECT_EQ(b.symbol, "SPLITDIV");
    }
    // Hand-computed: the split halves everything before row 6, the dividend scales
    // rows 1-2 by 102/102.5 on top of that; the newest bar is untouched.
    EXPECT_NEAR(bars.value()[7].close.as_double(), 54.0, 1e-7);
    EXPECT_NEAR(bars.value()[4].close.as_double(), 52.0, 1e-7);
    EXPECT_NEAR(bars.value()[2].close.as_double(), 51.0, 1e-7);
    EXPECT_NEAR(bars.value()[0].close.as_double(), 100.0 * 0.5 * (102.0 / 102.5), 1e-7);
}

TEST_F(CsvBarSourceTest, AnchorsAtLastBarWithinEndDate) {
    // Window ends before the split: rows 6-8 are neither returned nor allowed to
    // influence the factors, so row 5 is the anchor (factor 1) and only the dividend
    // still scales rows 1-2.
    auto bars = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 2), utc(2024, 1, 8));
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    ASSERT_EQ(bars.value().size(), 5u);
    EXPECT_NEAR(bars.value()[4].close.as_double(), 104.0, 1e-7);
    EXPECT_NEAR(bars.value()[2].close.as_double(), 102.0, 1e-7);
    EXPECT_NEAR(bars.value()[1].close.as_double(), 101.0 * (102.0 / 102.5), 1e-7);
}

TEST_F(CsvBarSourceTest, WarmupAndTradingWindowAreOneContinuousSeries) {
    // One load over [row1, row8] is continuous. Two separate loads, [row1,row4] and
    // [row5,row8], are each anchored at their own last bar, so the first half sits on a
    // different scale by the cumulative in-window factor (0.5 from the split) -- this is
    // the splice hazard a separate warmup pre-load would create.
    auto whole = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 2), utc(2024, 1, 11)).value();
    auto head = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 2), utc(2024, 1, 5)).value();
    auto tail = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 8), utc(2024, 1, 11)).value();
    ASSERT_EQ(head.size(), 4u);
    ASSERT_EQ(tail.size(), 4u);
    EXPECT_NEAR(whole[3].close.as_double(), 51.5, 1e-7);
    EXPECT_NEAR(head[3].close.as_double(), 103.0, 1e-7);
    EXPECT_NEAR(head[3].close.as_double() / whole[3].close.as_double(), 2.0, 1e-7);
    EXPECT_NEAR(tail[0].close.as_double(), whole[4].close.as_double(), 1e-7)
        << "the tail shares the whole-window anchor";
}

TEST_F(CsvBarSourceTest, RowsOutsideWindowDropped) {
    auto bars = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 4), utc(2024, 1, 9));
    ASSERT_TRUE(bars.is_ok()) << bars.error()->what();
    ASSERT_EQ(bars.value().size(), 4u);
    EXPECT_EQ(bars.value().front().timestamp, utc(2024, 1, 4));
    EXPECT_EQ(bars.value().back().timestamp, utc(2024, 1, 9));
}

TEST_F(CsvBarSourceTest, TimestampsAreUtcMidnight) {
    auto bars = load_csv_bars("SPLITDIV", dir_, utc(2024, 1, 2), utc(2024, 1, 11)).value();
    for (const auto& b : bars) {
        EXPECT_EQ(std::chrono::system_clock::to_time_t(b.timestamp) % 86400, 0);
    }
    EXPECT_EQ(core::format_utc_date(bars.front().timestamp), "2024-01-02");
}

TEST_F(CsvBarSourceTest, EmptyWindowIsDataNotFound) {
    auto bars = load_csv_bars("SPLITDIV", dir_, utc(2025, 1, 1), utc(2025, 2, 1));
    ASSERT_TRUE(bars.is_error());
    EXPECT_EQ(bars.error()->code(), ErrorCode::DATA_NOT_FOUND);
}

TEST_F(CsvBarSourceTest, MissingFileIsDataNotFound) {
    auto bars = load_csv_bars("NOPE", dir_, utc(2024, 1, 2), utc(2024, 1, 11));
    ASSERT_TRUE(bars.is_error());
    EXPECT_EQ(bars.error()->code(), ErrorCode::DATA_NOT_FOUND);
    EXPECT_NE(std::string(bars.error()->what()).find("NOPE.csv"), std::string::npos);
}

TEST_F(CsvBarSourceTest, RejectsVendorAdjustedHeader) {
    write("ADJ",
          "date,open,high,low,close,adj_close,volume\n"
          "2024-01-02,100,101,99,100,99.5,1000\n");
    auto rows = read_csv_bar_file(csv_bar_path(dir_, "ADJ"));
    ASSERT_TRUE(rows.is_error());
    EXPECT_EQ(rows.error()->code(), ErrorCode::INVALID_DATA);
    EXPECT_NE(std::string(rows.error()->what()).find("vendor-adjusted"), std::string::npos);
}

TEST_F(CsvBarSourceTest, RejectsWrongHeader) {
    write("HDR", "date,open,high,low,close,volume\n2024-01-02,100,101,99,100,1000\n");
    auto rows = read_csv_bar_file(csv_bar_path(dir_, "HDR"));
    ASSERT_TRUE(rows.is_error());
    EXPECT_EQ(rows.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(CsvBarSourceTest, RejectsUnsortedOrDuplicateDates) {
    write("DUP",
          "date,open,high,low,close,volume,div_cash,split_factor\n"
          "2024-01-03,100,101,99,100,1000,0,1\n"
          "2024-01-02,100,101,99,100,1000,0,1\n");
    auto unsorted = read_csv_bar_file(csv_bar_path(dir_, "DUP"));
    ASSERT_TRUE(unsorted.is_error());
    EXPECT_EQ(unsorted.error()->code(), ErrorCode::INVALID_DATA);

    write("DUP",
          "date,open,high,low,close,volume,div_cash,split_factor\n"
          "2024-01-02,100,101,99,100,1000,0,1\n"
          "2024-01-02,100,101,99,100,1000,0,1\n");
    auto dup = read_csv_bar_file(csv_bar_path(dir_, "DUP"));
    ASSERT_TRUE(dup.is_error());
    EXPECT_EQ(dup.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(CsvBarSourceTest, RejectsNonPositiveCloseOrSplit) {
    write("BAD",
          "date,open,high,low,close,volume,div_cash,split_factor\n"
          "2024-01-02,100,101,99,0,1000,0,1\n");
    EXPECT_TRUE(read_csv_bar_file(csv_bar_path(dir_, "BAD")).is_error());
    write("BAD",
          "date,open,high,low,close,volume,div_cash,split_factor\n"
          "2024-01-02,100,101,99,100,1000,0,0\n");
    EXPECT_TRUE(read_csv_bar_file(csv_bar_path(dir_, "BAD")).is_error());
    write("BAD",
          "date,open,high,low,close,volume,div_cash,split_factor\n"
          "2024-01-02,100,101,99,100,1000,-1,1\n");
    EXPECT_TRUE(read_csv_bar_file(csv_bar_path(dir_, "BAD")).is_error());
}
