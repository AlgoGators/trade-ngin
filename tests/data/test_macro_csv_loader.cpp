// tests/data/test_macro_csv_loader.cpp
//
// MacroCSVLoader reads BPGV's monthly macro CSV by column position: ten required columns
// through strong_risk_on, then optional GARCH columns that default when absent or empty.
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "../core/test_base.hpp"
#include "trade_ngin/data/macro_csv_loader.hpp"

using namespace trade_ngin;
using namespace trade_ngin::testing;

namespace {

const char* kHeader =
    "year,month,bpgv,bpgv_ewma,bpgv_percentile,yield_curve_spread,ewma_slope,regime_score,"
    "permit_growth,strong_risk_on,bpg_sfh,bpgv_sfh,bpgv_garch,bpgv_garch_percentile,"
    "garch_converged,garch_alpha,garch_beta\n";

class MacroCSVLoaderTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        path_ = std::filesystem::temp_directory_path() /
                ("macro_csv_loader_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<uintptr_t>(this)) + ".csv");
    }
    void TearDown() override {
        std::filesystem::remove(path_);
        TestBase::TearDown();
    }
    Result<std::vector<MonthlyMacroRecord>> load(const std::string& content) {
        std::ofstream(path_) << content;
        return MacroCSVLoader::load(path_.string());
    }
    std::filesystem::path path_;
};

}  // namespace

TEST_F(MacroCSVLoaderTest, ParsesRequiredAndGarchColumnsAndSortsByMonth) {
    auto r = load(std::string(kHeader) +
                  "2024,2,0.1,0.2,60,0.5,-0.01,0.25,0.02,0,0.03,0.04,0.05,80,1,0.1,0.85\r\n"
                  "2024,1,0.1,0.2,55,0.4,0.01,-0.5,-0.01,1,0.01,0.02,0.03,40,0,0.2,0.7\r\n");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    const auto& rows = r.value();
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].month, 1) << "rows come back sorted by (year, month)";
    EXPECT_DOUBLE_EQ(rows[0].regime_score, -0.5);
    EXPECT_TRUE(rows[0].strong_risk_on);
    EXPECT_DOUBLE_EQ(rows[1].bpgv_garch, 0.05);
    EXPECT_DOUBLE_EQ(rows[1].bpgv_garch_percentile, 80.0);
    EXPECT_TRUE(rows[1].garch_converged);
    EXPECT_DOUBLE_EQ(rows[1].garch_beta, 0.85) << "the CRLF line end is not part of the value";
}

TEST_F(MacroCSVLoaderTest, AbsentOrEmptyOptionalColumnsDefaultAndExtraColumnsAreIgnored) {
    auto r = load(std::string(kHeader) +
                  "2024,1,0.1,0.2,55,0.4,0.01,0.5,0.01,1\n"
                  "2024,2,0.1,0.2,55,0.4,0.01,0.5,0.01,0,0.01,,0.03,,1,0.2,0.7,99.5,7\n");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value()[0].bpgv_garch, 0.0) << "a row ending at strong_risk_on loads";
    EXPECT_DOUBLE_EQ(r.value()[1].bpgv_sfh, 0.0);
    EXPECT_DOUBLE_EQ(r.value()[1].bpgv_garch, 0.03);
    EXPECT_DOUBLE_EQ(r.value()[1].bpgv_garch_percentile, 0.0);
    EXPECT_DOUBLE_EQ(r.value()[1].garch_beta, 0.7);
}

TEST_F(MacroCSVLoaderTest, AMalformedValueIsAnErrorNamingTheLine) {
    auto required = load(std::string(kHeader) + "2024,1,0.1,0.2,55,0.4,0.01,x,0.01,1\n");
    ASSERT_TRUE(required.is_error());
    EXPECT_EQ(required.error()->code(), ErrorCode::INVALID_DATA);
    EXPECT_NE(std::string(required.error()->what()).find("line 2"), std::string::npos);

    auto optional = load(std::string(kHeader) + "2024,1,0.1,0.2,55,0.4,0.01,0.5,0.01,1,0.01,0.02,n/a\n");
    ASSERT_TRUE(optional.is_error()) << "a malformed GARCH value must not silently become 0";
    EXPECT_NE(std::string(optional.error()->what()).find("bpgv_garch"), std::string::npos);

    auto short_row = load(std::string(kHeader) + "2024,1,0.1,0.2,55\n");
    ASSERT_TRUE(short_row.is_error());
    EXPECT_EQ(short_row.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(MacroCSVLoaderTest, MissingEmptyAndHeaderOnlyFilesAreErrors) {
    auto missing = MacroCSVLoader::load((path_.parent_path() / "no_such_macro.csv").string());
    ASSERT_TRUE(missing.is_error());
    EXPECT_EQ(missing.error()->code(), ErrorCode::FILE_NOT_FOUND);

    auto empty = load("");
    ASSERT_TRUE(empty.is_error());
    EXPECT_EQ(empty.error()->code(), ErrorCode::INVALID_DATA);

    auto header_only = load(kHeader);
    ASSERT_TRUE(header_only.is_error());
    EXPECT_EQ(header_only.error()->code(), ErrorCode::INVALID_DATA);
}

TEST(MacroCSVLoaderFind, ExactMonthAndTheLatestAtOrBefore) {
    std::vector<MonthlyMacroRecord> rows(3);
    rows[0].year = 2023, rows[0].month = 11;
    rows[1].year = 2023, rows[1].month = 12;
    rows[2].year = 2024, rows[2].month = 2;

    ASSERT_TRUE(MacroCSVLoader::find_record(rows, 2023, 12).has_value());
    EXPECT_FALSE(MacroCSVLoader::find_record(rows, 2024, 1).has_value());

    auto before = MacroCSVLoader::find_record_before(rows, 2024, 1);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(before->month, 12) << "January 2024 falls back to December 2023";
    EXPECT_EQ(MacroCSVLoader::find_record_before(rows, 2024, 2)->month, 2);
    EXPECT_FALSE(MacroCSVLoader::find_record_before(rows, 2023, 10).has_value());
    EXPECT_FALSE(MacroCSVLoader::find_record_before({}, 2024, 1).has_value());
}
