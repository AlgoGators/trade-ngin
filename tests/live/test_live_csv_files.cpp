// The two CSV files a futures live run writes (src/live/csv_exporter.cpp), read back as a reader
// meets them:
//   - <date>_positions.csv: comment lines, then the column names, then one row per sleeve and
//     symbol. The sleeve's sizing volatility is the column sizing_vol, explained on a comment line;
//     every row prints six decimals, the first row included; a fast sleeve's row carries its own
//     forecast, sizing volatility and moving averages.
//   - <day before>_positions_asof_<date>.csv: the column names, then one row per settled position,
//     six decimals on every row.
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
            EXPECT_TRUE(std::regex_match(cells[c], kSixDecimals))
                << "row " << (i - h) << " column " << c << " prints '" << cells[c]
                << "': " << lines[i];
        }
        EXPECT_EQ(cells[3], "110.906250") << "the mark, every decimal of the format";
    }
}

// Landed before this series (one trend class since the fast and slow classes were removed): the
// fast sleeve's row is filled as the trend sleeve's is. Columns by position, so the test reads
// the same on the parent.
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
    // Row 2 onward printed six decimals before this series as well; the first row did not, so
    // the figures are compared as numbers to the precision the parent's first row has.
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

TEST_F(LiveCsvFiles, EveryRowOfTheSettledFilePrintsSixDecimals) {
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
    for (size_t c = 2; c < cells.size(); ++c) {
        EXPECT_TRUE(std::regex_match(cells[c], kSixDecimals)) << "column " << c << ": " << lines[1];
    }
    EXPECT_EQ(cells[4], "110.906250") << "a price with five decimals lost two at six digits";
    EXPECT_EQ(cells[5], "-185.000000");
}
