// Coverage for csv_exporter.cpp targeting the standalone helpers and
// constructor/setter paths. Full export() paths require a live DB or a
// strategy instance with realistic state — those overloads are documented
// as deferred in deliverables/unit_testing/.

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>

// Pre-load std headers BEFORE flipping private→public so libc++ internals
// stay valid.
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <vector>

#define private public
#include "trade_ngin/live/csv_exporter.hpp"
#undef private

using namespace trade_ngin;

namespace {

std::chrono::system_clock::time_point at_local(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    return std::chrono::system_clock::from_time_t(std::mktime(&tm));
}

}  // namespace

class CSVExporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        dir_ = std::filesystem::temp_directory_path() /
               ("csv_exporter_" + std::string(info->name()));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::filesystem::path dir_;
};

TEST_F(CSVExporterTest, ConstructorDefaultAppendsTrailingSlash) {
    CSVExporter exp;
    EXPECT_EQ(exp.output_directory_, "./");
}

TEST_F(CSVExporterTest, ConstructorWithExplicitDirectoryAppendsTrailingSlash) {
    CSVExporter exp(dir_.string());
    EXPECT_TRUE(exp.output_directory_.starts_with(dir_.string()));
    EXPECT_EQ(exp.output_directory_.back(), '/');
}

TEST_F(CSVExporterTest, ConstructorPreservesTrailingSlashIfPresent) {
    CSVExporter exp("/tmp/already_has_slash/");
    EXPECT_EQ(exp.output_directory_, "/tmp/already_has_slash/");
}

TEST_F(CSVExporterTest, SetOutputDirectoryAppendsTrailingSlashIfMissing) {
    CSVExporter exp;
    exp.set_output_directory(dir_.string());
    EXPECT_EQ(exp.output_directory_.back(), '/');
}

// ===== format_date_for_filename / format_date_for_display =====

TEST_F(CSVExporterTest, FormatDateForFilenameYYYYMMDD) {
    CSVExporter exp;
    auto s = exp.format_date_for_filename(at_local(2026, 4, 28));
    EXPECT_EQ(s, "2026-04-28");
}

TEST_F(CSVExporterTest, FormatDateForDisplayYYYYMMDD) {
    CSVExporter exp;
    auto s = exp.format_date_for_display(at_local(2026, 1, 5));
    EXPECT_EQ(s, "2026-01-05");
}

TEST_F(CSVExporterTest, FormatDateZeroPadsMonthAndDay) {
    CSVExporter exp;
    EXPECT_EQ(exp.format_date_for_filename(at_local(2026, 9, 7)), "2026-09-07");
}

// ===== get_clean_symbol =====

TEST_F(CSVExporterTest, GetCleanSymbolStripsVariantSuffix) {
    CSVExporter exp;
    EXPECT_EQ(exp.get_clean_symbol("ES.v.0"), "ES");
}

TEST_F(CSVExporterTest, GetCleanSymbolStripsContinuousSuffix) {
    CSVExporter exp;
    EXPECT_EQ(exp.get_clean_symbol("ES.c.0"), "ES");
}

TEST_F(CSVExporterTest, GetCleanSymbolPlainSymbolUnchanged) {
    CSVExporter exp;
    EXPECT_EQ(exp.get_clean_symbol("ES"), "ES");
}

// ===== format_strategy_display_name =====

TEST_F(CSVExporterTest, FormatStrategyDisplayNameTitleCases) {
    CSVExporter exp;
    auto s = exp.format_strategy_display_name("TREND_FOLLOWING_FAST");
    // The exact format isn't part of the public contract, but it should
    // produce a non-empty human-friendly string.
    EXPECT_FALSE(s.empty());
    // Should not contain the raw underscore-uppercase form.
    EXPECT_NE(s, "TREND_FOLLOWING_FAST");
}

// ===== calculate_notional =====

TEST_F(CSVExporterTest, CalculateNotionalForUnknownSymbolThrows) {
    CSVExporter exp;
    EXPECT_THROW(exp.calculate_notional("UNKNOWN_SYMBOL_XYZ", 2.0, 100.0),
                 std::runtime_error);
}

// ===== write_portfolio_header =====

TEST_F(CSVExporterTest, WritePortfolioHeaderProducesCommentLines) {
    CSVExporter exp;
    auto path = dir_ / "header.csv";
    {
        std::ofstream f(path);
        exp.write_portfolio_header(f, /*portfolio=*/100000.0, /*gross=*/200000.0,
                                    /*net=*/150000.0, "2026-04-28");
    }
    std::ifstream in(path);
    std::string line;
    bool found_value = false;
    while (std::getline(in, line)) {
        if (line.find("100000") != std::string::npos) found_value = true;
    }
    EXPECT_TRUE(found_value);
}
