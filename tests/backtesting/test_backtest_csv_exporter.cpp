#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_csv_exporter.hpp"
#include "trade_ngin/strategy/trend_following.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Position make_pos(const std::string& sym, double qty, double avg_price = 100.0) {
    return Position(sym, Quantity(qty), Price(avg_price), Decimal(0.0), Decimal(0.0),
                    Timestamp{});
}

Timestamp date_at(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;  // mid-day to avoid timezone day shifts in format_date
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::stringstream buf;
    buf << f.rdbuf();
    return buf.str();
}

}  // namespace

class BacktestCSVExporterTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        // Per-test unique temp dir to keep tests isolated.
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        out_dir_ = std::filesystem::temp_directory_path() /
                   ("trade_ngin_csv_exporter_" + std::string(info->name()));
        std::filesystem::remove_all(out_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(out_dir_);
        TestBase::TearDown();
    }

    std::filesystem::path out_dir_;
};

TEST_F(BacktestCSVExporterTest, InitializeCreatesDirectoryAndAllThreeFilesWithHeaders) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.initialize_files();
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "positions.csv"));
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "finalized_positions.csv"));
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "equity_curve.csv"));

    // finalize() flushes & closes the streams so the headers land on disk.
    exporter.finalize();
    EXPECT_NE(slurp(out_dir_ / "positions.csv").find("date,symbol,quantity"), std::string::npos);
    EXPECT_NE(slurp(out_dir_ / "finalized_positions.csv").find("realized_pnl"),
              std::string::npos);
    EXPECT_EQ(slurp(out_dir_ / "equity_curve.csv"), "date,portfolio_value\n");
}

TEST_F(BacktestCSVExporterTest, InitializeFailsOnUnwritablePath) {
    // The path has an embedded null which std::filesystem rejects.
    BacktestCSVExporter exporter(std::string("/proc/this_path_does_not_exist_anywhere\0bad", 45));
    auto r = exporter.initialize_files();
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::CONVERSION_ERROR);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsWithoutInitializeReturnsError) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.append_daily_positions(date_at(2026, 1, 5), {}, {}, 1.0e6, 1.0, 1.0, {});
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsWritesPortfolioHeaderEvenWithNoPositions) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), {}, {}, 1'000'000.0, 0.0, 0.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("# Portfolio Value: 1e+06"), std::string::npos);
    EXPECT_NE(body.find("Date: 2026-01-05"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsSkipsZeroQuantityPositions) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 0.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), pos, prices, 1'000'000.0,
                                             100.0, 100.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    // Header + portfolio comment, but no data row for ES.
    int line_count = 0;
    for (char c : body) if (c == '\n') ++line_count;
    EXPECT_EQ(line_count, 2);  // header + portfolio comment
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsRowsContainSymbolAndComputedFields) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 5.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    // No instrument registry → notional = qty * price = 20000
    // gross_notional=20000 → pct_gross = 1.0
    // portfolio_value=1e6 → pct_portfolio = 0.02
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), pos, prices, 1'000'000.0,
                                             20000.0, 20000.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("ES,5,4000,20000"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsHandlesZeroPortfolioValueAndGrossNotional) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 1.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 100.0}};
    // gross_notional=0 → pct_gross=0; portfolio_value=0 → pct_portfolio=0
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), pos, prices, 0.0, 0.0, 0.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("ES,1,100"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsHandlesMissingMarketPrice) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 5.0)}};
    std::unordered_map<std::string, double> prices;  // empty
    ASSERT_TRUE(exporter
                    .append_daily_positions(date_at(2026, 1, 5), pos, prices, 1.0e6,
                                             1.0, 1.0, {})
                    .is_ok());
    std::string body = slurp(out_dir_ / "positions.csv");
    EXPECT_NE(body.find("ES,5,0"), std::string::npos);  // price defaulted to 0
}

TEST_F(BacktestCSVExporterTest, AppendDailyPositionsAcceptsEmptyStrategyVector) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> pos = {{"ES", make_pos("ES", 3.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    std::vector<std::shared_ptr<StrategyInterface>> strategies;
    auto r = exporter.append_daily_positions(date_at(2026, 1, 5), pos, prices, 1.0e6,
                                              1.0, 1.0, strategies);
    EXPECT_TRUE(r.is_ok());
}

TEST_F(BacktestCSVExporterTest, AppendEquityCurveWithoutInitializeReturnsError) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.append_equity_curve(date_at(2026, 1, 5), 1'000'000.0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestCSVExporterTest, AppendEquityCurveWritesDateAndValue) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    ASSERT_TRUE(exporter.append_equity_curve(date_at(2026, 1, 5), 1'234'567.0).is_ok());
    ASSERT_TRUE(exporter.append_equity_curve(date_at(2026, 1, 6), 1'250'000.0).is_ok());
    std::string body = slurp(out_dir_ / "equity_curve.csv");
    EXPECT_NE(body.find("2026-01-05,1.23457e+06"), std::string::npos);
    EXPECT_NE(body.find("2026-01-06,1.25e+06"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsWithoutInitializeReturnsError) {
    BacktestCSVExporter exporter(out_dir_.string());
    auto r = exporter.append_finalized_positions(date_at(2026, 1, 5), {}, {}, {});
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsSkipsUnchangedPositions) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr = {{"ES", make_pos("ES", 5.0, 4000.0)}};
    std::unordered_map<std::string, Position> prev = {{"ES", make_pos("ES", 5.0, 4000.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4010.0}};
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    int line_count = 0;
    for (char c : body) if (c == '\n') ++line_count;
    EXPECT_EQ(line_count, 1);  // header only
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsRecordsNewEntry) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr = {{"ES", make_pos("ES", 5.0)}};
    std::unordered_map<std::string, Position> prev;
    std::unordered_map<std::string, double> prices = {{"ES", 4000.0}};
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    // New entry: entry_price = current market price (4000), exit_price = 0, pnl = 0
    EXPECT_NE(body.find("ES,5,4000,0,0"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsRecordsClosedTradeWithRealizedPnL) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr = {{"ES", make_pos("ES", 0.0, 0.0)}};
    std::unordered_map<std::string, Position> prev = {{"ES", make_pos("ES", 5.0, 4000.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4010.0}};
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    // No instrument registry → multiplier = 1; closed_qty = 5; realized = 5*(4010-4000)*1 = 50
    EXPECT_NE(body.find("ES,0,4000,4010,50"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsRecordsReducedTradeRealizedPnL) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr = {{"ES", make_pos("ES", 2.0, 4000.0)}};
    std::unordered_map<std::string, Position> prev = {{"ES", make_pos("ES", 5.0, 4000.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4010.0}};
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    // closed_qty = 5 - 2 = 3; realized = 3*(4010-4000)*1 = 30
    EXPECT_NE(body.find("ES,2,4000,4010,30"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsHandlesIncreasedPositionWithoutPnL) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr = {{"ES", make_pos("ES", 8.0, 4000.0)}};
    std::unordered_map<std::string, Position> prev = {{"ES", make_pos("ES", 5.0, 4000.0)}};
    std::unordered_map<std::string, double> prices = {{"ES", 4010.0}};
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    // |curr| > |prev| → no realized PnL branch; entry comes from prev.average_price
    EXPECT_NE(body.find("ES,8,4000,4010,0"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsCoversSymbolsOnlyInPrevious) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr;  // empty
    std::unordered_map<std::string, Position> prev = {{"NQ", make_pos("NQ", 3.0, 15000.0)}};
    std::unordered_map<std::string, double> prices = {{"NQ", 15100.0}};
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    // closed_qty = 3 - 0 = 3; realized = 3*(15100-15000)*1 = 300
    EXPECT_NE(body.find("NQ,0,15000,15100,300"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, AppendFinalizedPositionsHandlesMissingMarketPrice) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    std::unordered_map<std::string, Position> curr = {{"ES", make_pos("ES", 0.0)}};
    std::unordered_map<std::string, Position> prev = {{"ES", make_pos("ES", 5.0, 4000.0)}};
    std::unordered_map<std::string, double> prices;  // no entry
    ASSERT_TRUE(exporter
                    .append_finalized_positions(date_at(2026, 1, 5), curr, prev, prices)
                    .is_ok());
    std::string body = slurp(out_dir_ / "finalized_positions.csv");
    EXPECT_NE(body.find("ES,0,4000,0"), std::string::npos);
}

TEST_F(BacktestCSVExporterTest, FinalizeIsIdempotentAndSafeAfterDestructor) {
    {
        BacktestCSVExporter exporter(out_dir_.string());
        ASSERT_TRUE(exporter.initialize_files().is_ok());
        exporter.finalize();
        // Second call should be a no-op without crashing.
        exporter.finalize();
    }  // destructor runs finalize() again
    EXPECT_TRUE(std::filesystem::exists(out_dir_ / "positions.csv"));
}

TEST_F(BacktestCSVExporterTest, AppendsAfterFinalizeReturnError) {
    BacktestCSVExporter exporter(out_dir_.string());
    ASSERT_TRUE(exporter.initialize_files().is_ok());
    exporter.finalize();
    EXPECT_TRUE(exporter.append_daily_positions(date_at(2026, 1, 5), {}, {}, 1.0, 1.0, 1.0, {})
                    .is_error());
    EXPECT_TRUE(exporter.append_equity_curve(date_at(2026, 1, 5), 1.0).is_error());
    EXPECT_TRUE(exporter.append_finalized_positions(date_at(2026, 1, 5), {}, {}, {}).is_error());
}
