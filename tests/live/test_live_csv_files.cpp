// The two CSV files a futures live run writes (src/live/csv_exporter.cpp), read back as a reader
// meets them:
//   - <date>_positions.csv: comment lines, then the column names, then one row per sleeve and
//     symbol. The sleeve's sizing volatility is the column sizing_vol, explained on a comment line;
//     every row prints six decimals, the first row included, and its market price in full (the
//     shortest text that reads back as the same number); a fast sleeve's row carries its own
//     forecast, sizing volatility and moving averages.
//     The first line and a comment line say the portfolio value is the value as this run wrote it.
//   - <day before>_positions_asof_<date>.csv: the column names, then one row per settled position,
//     six decimals on every row; its two prices are the prior mark and the mark, each printed in
//     full (a 6J mark of 0.0062995 keeps its seventh decimal), and a symbol with no close has an
//     empty cell.
//   - the carried file's description of a stored forecast (carried_forecast_label).
// Every test here calls the exporter. No database.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../strategy/csv_test_sleeve.hpp"
#include "trade_ngin/core/logger.hpp"
#include "trade_ngin/core/report_settled_marks.hpp"
#include "trade_ngin/live/carried_day.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#include "trade_ngin/strategy/base_strategy.hpp"
#include "trade_ngin/strategy/trend_readings.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const std::string kSym = "CSVLIVE";

std::chrono::system_clock::time_point utc_day(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = 5;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

Position held(const std::string& symbol, double qty, double price, double realized = 0.0) {
    Position p;
    p.symbol = symbol;
    p.quantity = Decimal(qty);
    p.average_price = Decimal(price);
    p.realized_pnl = Decimal(realized);
    return p;
}

std::vector<std::string> lines_of(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    std::string line;
    while (std::getline(in, line))
        out.push_back(line);
    return out;
}

std::vector<std::string> cells_of(const std::string& line) {
    std::vector<std::string> out;
    std::string cell;
    std::istringstream in(line);
    while (std::getline(in, cell, ','))
        out.push_back(cell);
    if (!line.empty() && line.back() == ',')
        out.push_back("");
    return out;
}

std::string six(double v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(6) << v;
    return ss.str();
}

const std::regex kSixDecimals(R"(^-?[0-9]+\.[0-9]{6}$)");

class LiveCsvFiles : public ::testing::Test {
protected:
    void SetUp() override {
        LoggerConfig lc;
        lc.destination = LogDestination::CONSOLE;
        lc.min_level = LogLevel::ERR;
        lc.include_timestamp = false;
        Logger::instance().initialize(lc);
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               ("live_csv_files_" + std::string(info->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        trend_ = std::make_unique<CsvTestSleeve>("TREND_FOLLOWING", kSym, TrendFollowingConfig{});
        fast_ = std::make_unique<CsvTestSleeve>("TREND_FOLLOWING_FAST", kSym,
                                                fast_trend_following_config());
    }
    void TearDown() override {
        std::filesystem::remove_all(dir_);
    }

    // The day's positions file of a book of two sleeves, both holding kSym.
    std::vector<std::string> positions_file() {
        StrategyPositionsMap book{{"TREND_FOLLOWING", {{kSym, held(kSym, 1.0, 110.90625)}}},
                                  {"TREND_FOLLOWING_FAST", {{kSym, held(kSym, -2.0, 110.90625)}}}};
        StrategyInstancesMap sleeves{{"TREND_FOLLOWING", trend_->strategy.get()},
                                     {"TREND_FOLLOWING_FAST", fast_->strategy.get()}};
        CSVExporter exporter(dir_.string());
        auto r = exporter.export_current_positions(utc_day(2026, 4, 24), book, {{kSym, 110.90625}},
                                                   497400.90, 1663.59375, -554.53125, sleeves);
        EXPECT_TRUE(r.is_ok());
        return r.is_ok() ? lines_of(r.value()) : std::vector<std::string>{};
    }

    // The index of the column names line: the first line that is not a comment.
    static size_t header_at(const std::vector<std::string>& lines) {
        size_t i = 0;
        while (i < lines.size() && !lines[i].empty() && lines[i][0] == '#')
            ++i;
        return i;
    }

    std::filesystem::path dir_;
    std::unique_ptr<CsvTestSleeve> trend_;
    std::unique_ptr<CsvTestSleeve> fast_;
};

}  // namespace

TEST_F(LiveCsvFiles, ThePositionsFileNamesTheSizingVolatilityAndExplainsItOnACommentLine) {
    const auto lines = positions_file();
    const size_t h = header_at(lines);
    ASSERT_LT(h, lines.size());
    const auto names = cells_of(lines[h]);
    ASSERT_EQ(names.size(), 13u) << lines[h];
    EXPECT_EQ(names[8], "sizing_vol") << "the sleeve's sizing estimate under the name of the "
                                         "account's realised volatility statistic";
    for (const auto& name : names)
        EXPECT_NE(name, "volatility") << lines[h];
    // The explanation sits on a comment line above the column names, where the file already
    // carries a comment line: a reader that skips '#' lines finds the column names unchanged.
    ASSERT_GE(h, 2u) << "no comment line explains sizing_vol";
    bool explained = false;
    for (size_t i = 0; i < h; ++i) {
        if (lines[i].rfind("# sizing_vol: ", 0) == 0) {
            explained = true;
            EXPECT_NE(lines[i].find("annualised"), std::string::npos) << lines[i];
            EXPECT_NE(lines[i].find("not the account's realised volatility"), std::string::npos)
                << lines[i];
        }
    }
    EXPECT_TRUE(explained);
}

TEST_F(LiveCsvFiles, EveryRowOfThePositionsFilePrintsSixDecimalsTheFirstRowIncluded) {
    const auto lines = positions_file();
    const size_t h = header_at(lines);
    ASSERT_EQ(lines.size(), h + 3) << "two sleeves, one symbol each";
    for (size_t i = h + 1; i < lines.size(); ++i) {
        const auto cells = cells_of(lines[i]);
        ASSERT_EQ(cells.size(), 13u) << lines[i];
        for (size_t c = 2; c < cells.size(); ++c) {
            if (c == 3 || c == 2 || c == 4)
                continue;  // quantity, market_price and notional: the one number rule, below
            EXPECT_TRUE(std::regex_match(cells[c], kSixDecimals))
                << "row " << (i - h) << " column " << c << " prints '" << cells[c]
                << "': " << lines[i];
        }
        EXPECT_EQ(cells[3], "110.9062") << "the price at the decimals of its contract";
        EXPECT_TRUE(std::regex_match(cells[2], std::regex("-?[0-9]+\\.[0-9]{2}"))) << "a quantity has two decimals";
        EXPECT_TRUE(std::regex_match(cells[4], std::regex("-?[0-9]+\\.[0-9]{2}"))) << "money has two decimals";
    }
}

// A price below 0.1 with seven decimals (a 6J close): six decimals cut its last digit. Every
// price column of the three live files prints the shortest text that reads back as the number.
TEST_F(LiveCsvFiles, TheSettledFileKeepsEveryDigitOfASmallMark) {
    const std::string yen = "6J.v.0";
    StrategyPositionsMap settled{{"TREND_FOLLOWING", {{yen, held(yen, -1.0, 0.0063125, 162.5)}}}};
    CSVExporter exporter(dir_.string());
    auto r = exporter.export_finalized_positions(utc_day(2026, 4, 28), utc_day(2026, 4, 27),
                                                 settled, {{yen, 0.0063125}}, {{yen, 0.0062995}});
    ASSERT_TRUE(r.is_ok());
    const auto lines = lines_of(r.value());
    ASSERT_EQ(lines.size(), 2u);
    const auto cells = cells_of(lines[1]);
    ASSERT_EQ(cells.size(), 6u) << lines[1];
    // Four decimals on every price, so a 6J close reads 0.0063
    EXPECT_EQ(cells[3], "0.0063") << lines[1];
    EXPECT_EQ(cells[4], "0.0063") << lines[1];
    EXPECT_EQ(cells[2], "-1.00");
    EXPECT_EQ(cells[5], "162.50");
}

TEST_F(LiveCsvFiles, ThePositionsFileAndTheCarriedFileKeepEveryDigitOfASmallMarketPrice) {
    StrategyPositionsMap book{{"TREND_FOLLOWING", {{kSym, held(kSym, -1.0, 0.0062995)}}}};
    StrategyInstancesMap sleeves{{"TREND_FOLLOWING", trend_->strategy.get()}};
    CSVExporter exporter(dir_.string());
    auto day = exporter.export_current_positions(utc_day(2026, 4, 28), book, {{kSym, 0.0062995}},
                                                 500000.0, 100.0, -100.0, sleeves);
    ASSERT_TRUE(day.is_ok());
    const auto lines = lines_of(day.value());
    const size_t h = header_at(lines);
    ASSERT_EQ(lines.size(), h + 2);
    const auto cells = cells_of(lines[h + 1]);
    ASSERT_EQ(cells.size(), 13u) << lines[h + 1];
    EXPECT_EQ(cells[3], "0.0063") << "the contract's four decimals (this test contract has a cent tick)";

    auto carried = exporter.export_carried_positions(
        utc_day(2026, 4, 26), book, {{kSym, CarriedMark{0.0062995, "2026-04-24"}}}, {}, 500000.0,
        100.0, -100.0, "no session on 2026-04-25 (a Saturday)");
    ASSERT_TRUE(carried.is_ok());
    const auto carried_lines = lines_of(carried.value());
    const size_t ch = header_at(carried_lines);
    ASSERT_EQ(carried_lines.size(), ch + 2);
    const auto carried_cells = cells_of(carried_lines[ch + 1]);
    ASSERT_GE(carried_cells.size(), 14u) << carried_lines[ch + 1];
    EXPECT_EQ(carried_cells[3], "0.0063");
    EXPECT_EQ(carried_cells[2], "-1.00");
}

// One trend class: the fast sleeve's row is filled as the trend sleeve's is. Columns by
// position.
TEST_F(LiveCsvFiles, TheFastSleevesRowCarriesItsOwnForecastSizingVolatilityAndAverages) {
    const auto lines = positions_file();
    const size_t h = header_at(lines);
    std::map<std::string, std::vector<std::string>> row;
    for (size_t i = h + 1; i < lines.size(); ++i)
        row[cells_of(lines[i])[0]] = cells_of(lines[i]);
    ASSERT_EQ(row.count("Trend Following Fast"), 1u);
    ASSERT_EQ(row.count("Trend Following"), 1u);
    const auto& fast = row.at("Trend Following Fast");
    const auto& trend = row.at("Trend Following");
    const InstrumentData* data = fast_->strategy->get_instrument_data(kSym);
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->current_volatility, 0.01) << "the test sleeve computed no volatility";
    ASSERT_NE(data->current_forecast, 0.0) << "the test sleeve computed no forecast";
    // The first row's figures are compared as numbers, to the precision a row printed with
    // fewer decimals has.
    EXPECT_NEAR(std::stod(fast[7]), data->current_forecast, 0.005);
    EXPECT_EQ(fast[8], six(data->current_volatility));
    const auto emas = fast_->strategy->get_ema_values(kSym, {8, 32, 64, 256});
    EXPECT_EQ(fast[9], six(emas.at(8)));
    EXPECT_EQ(fast[10], six(emas.at(32)));
    EXPECT_EQ(fast[11], six(emas.at(64)));
    EXPECT_EQ(fast[12], six(emas.at(256)));
    EXPECT_NE(fast[8], trend[8]) << "the fast sleeve printed the trend sleeve's sizing volatility";
    // The one function both exporters read a sleeve through gives the same figures.
    const TrendReadings readings = trend_readings_of(fast_->strategy.get(), kSym);
    EXPECT_TRUE(readings.known);
    EXPECT_EQ(readings.sizing_vol, data->current_volatility);
    EXPECT_EQ(readings.forecast, data->current_forecast);
    EXPECT_FALSE(trend_readings_of(fast_->strategy.get(), "NOT_A_SYMBOL").known);
    EXPECT_FALSE(trend_readings_of(nullptr, kSym).known);
}

TEST_F(LiveCsvFiles, EveryRowOfTheSettledFilePrintsSixDecimalsAndItsMarksInFull) {
    StrategyPositionsMap settled{{"TREND_FOLLOWING", {{kSym, held(kSym, 1.0, 110.5, -185.0)}}}};
    CSVExporter exporter(dir_.string());
    auto r = exporter.export_finalized_positions(utc_day(2026, 4, 25), utc_day(2026, 4, 24),
                                                 settled, {{kSym, 110.5}}, {{kSym, 110.90625}});
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(std::filesystem::path(r.value()).filename(),
              "2026-04-24_positions_asof_2026-04-25.csv");
    const auto lines = lines_of(r.value());
    ASSERT_EQ(lines.size(), 2u);
    const auto cells = cells_of(lines[1]);
    ASSERT_EQ(cells.size(), 6u) << lines[1];
    EXPECT_EQ(cells[2], "1.00");
    EXPECT_EQ(cells[3], "110.5000");
    EXPECT_EQ(cells[4], "110.9062") << "the price at the decimals of its contract";
    EXPECT_EQ(cells[5], "-185.00");
}

// The settled file's prior_mark and mark are the two prices of the report's settled
// table, the pair the settle booked the row on (core/report_settled_marks.hpp). A Monday file
// settles Sunday: KE (1 contract; Thu 679.50, Fri 668.50, no Sunday session) printed no bar, so
// its mark is empty and its prior_mark is its last known close, Friday's 668.5, not Thursday's
// 679.5, which is what the bar before its newest bar gives. The column names do not change.
TEST_F(LiveCsvFiles, TheSettledFileOfANoBarRowCarriesTheLastKnownCloseAndAnEmptyMark) {
    const std::string wheat = "KE.v.0";
    StrategyPositionsMap settled{
        {"TREND_FOLLOWING",
         {{kSym, held(kSym, 2.0, 110.859375, 62.5)}, {wheat, held(wheat, 1.0, 668.5, 0.0)}}}};
    // the settle's own inputs: its "from" map (Thursday's close for KE, never used: no bar to
    // settle at), its "to" map (no KE), and the last known close of the symbol with no bar
    const SettledTablePrices table = settled_table_prices({{kSym, 110.859375}, {wheat, 679.5}},
                                                          {{kSym, 110.890625}}, {{wheat, 668.5}});
    CSVExporter exporter(dir_.string());
    auto r = exporter.export_finalized_positions(utc_day(2026, 4, 27), utc_day(2026, 4, 26),
                                                 settled, table.previous, table.latest);
    ASSERT_TRUE(r.is_ok());
    const auto lines = lines_of(r.value());
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "strategy,symbol,quantity,prior_mark,mark,realized_pnl");
    std::map<std::string, std::string> line_of;
    for (size_t i = 1; i < lines.size(); ++i)
        line_of[cells_of(lines[i])[1]] = lines[i];
    EXPECT_EQ(line_of.at(wheat), "Trend Following,KE.v.0,1.00,668.5000,,0.00");
    // the row beside it printed a bar: 2 x (110.890625 - 110.859375) x 1,000 = 62.50
    EXPECT_EQ(line_of.at(kSym),
              "Trend Following," + kSym + ",2.00,110.8594,110.8906,62.50");
    const auto cells = cells_of(line_of.at(kSym));
    EXPECT_NEAR(2.0 * (std::stod(cells[4]) - std::stod(cells[3])) * 1000.0, 62.5, 0.2);
}

TEST_F(LiveCsvFiles, ThePortfolioLineAndTheShareColumnSayTheValueIsAsWritten) {
    const auto lines = positions_file();
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines[0],
              "# Portfolio Value (as written: today's costs booked): 497400.90, Gross Notional: "
              "1663.59, Net Notional: -554.53, Date: 2026-04-24");
    const size_t h = header_at(lines);
    ASSERT_EQ(h, 3u) << "the portfolio line and one comment line per explained column";
    EXPECT_EQ(lines[2], "# " + CSVExporter::pct_of_portfolio_value_note());
    EXPECT_EQ(lines[2].rfind("# pct_of_portfolio_value: ", 0), 0u) << lines[2];
    EXPECT_NE(lines[2].find("(as written: today's costs booked)"), std::string::npos) << lines[2];
    // The share itself is unchanged: the row's notional over that value, in percent.
    for (size_t i = h + 1; i < lines.size(); ++i) {
        const auto cells = cells_of(lines[i]);
        ASSERT_EQ(cells.size(), 13u);
        EXPECT_NEAR(std::stod(cells[6]), std::abs(std::stod(cells[4])) / 497400.90 * 100.0, 2e-5) << lines[i];
    }
}

TEST_F(LiveCsvFiles, TheSettledFileNamesThePriorMarkAndTheMarkAndLeavesAMissingMarkEmpty) {
    const std::string other = "CSVLIVE2";
    StrategyPositionsMap settled{
        {"TREND_FOLLOWING",
         {{kSym, held(kSym, 1.0, 110.5, -185.0)}, {other, held(other, 2.0, 57.25, 0.0)}}}};
    CSVExporter exporter(dir_.string());
    // `other` printed no bar on the settled day: it has a prior mark and no mark.
    auto r =
        exporter.export_finalized_positions(utc_day(2026, 4, 25), utc_day(2026, 4, 24), settled,
                                            {{kSym, 110.5}, {other, 57.25}}, {{kSym, 110.90625}});
    ASSERT_TRUE(r.is_ok());
    const auto lines = lines_of(r.value());
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "strategy,symbol,quantity,prior_mark,mark,realized_pnl")
        << "the file starts with its column names; the two prices are marks, not an entry and "
           "an exit";
    std::map<std::string, std::vector<std::string>> row;
    for (size_t i = 1; i < lines.size(); ++i)
        row[cells_of(lines[i])[1]] = cells_of(lines[i]);
    ASSERT_EQ(row.at(kSym).size(), 6u);
    EXPECT_EQ(row.at(kSym)[3], "110.5000");
    EXPECT_EQ(row.at(kSym)[4], "110.9062");
    EXPECT_EQ(row.at(kSym)[5], "-185.00");
    ASSERT_EQ(row.at(other).size(), 6u);
    EXPECT_EQ(row.at(other)[3], "57.2500");
    EXPECT_EQ(row.at(other)[4], "") << "a symbol with no close that day printed a mark of 0";
    EXPECT_EQ(row.at(other)[5], "0.00");
}

TEST(CarriedForecastLabel, NamesTheRunAndTheLastBarDateItRead) {
    EXPECT_EQ(carried_forecast_label("2026-05-02"),
              "computed by the run of 2026-05-02 from bars through 2026-05-01");
    EXPECT_EQ(carried_forecast_label("2026-05-01"),
              "computed by the run of 2026-05-01 from bars through 2026-04-30");
    EXPECT_EQ(carried_forecast_label("2026-01-01"),
              "computed by the run of 2026-01-01 from bars through 2025-12-31");
    EXPECT_EQ(carried_forecast_label(""), "");
    EXPECT_EQ(carried_forecast_label("no date"), "computed by the run of no date");
}
