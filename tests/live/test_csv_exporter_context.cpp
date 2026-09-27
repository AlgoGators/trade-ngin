// Isolated local CSV tests: no EmailSender, strategy, database or market loader.
#include <gtest/gtest.h>
#include <arrow/api.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/instruments/equity.hpp"
#include "trade_ngin/instruments/futures.hpp"
#include "trade_ngin/instruments/option.hpp"
#include "trade_ngin/live/csv_exporter.hpp"
#define private public
#include "trade_ngin/instruments/instrument_registry.hpp"
#undef private

#ifndef _WIN32
#include <sys/stat.h>
using namespace trade_ngin;
namespace {
auto day() {
    return std::chrono::system_clock::time_point(
        std::chrono::sys_days(std::chrono::year{2026}/9/26)) + std::chrono::hours(12);
}
Position position(const std::string& symbol, int64_t raw) {
    return Position(symbol, Quantity::from_raw(raw), Price(10.0), Decimal(0.0),
                    Decimal(0.0), day());
}
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::vector<std::string> lines(const std::string& text) {
    std::istringstream input(text); std::vector<std::string> out; std::string line;
    while (std::getline(input,line)) out.push_back(line);
    return out;
}
class RegistryGuard {
    InstrumentRegistry& registry_ = InstrumentRegistry::instance();
    std::unordered_map<std::string,std::shared_ptr<Instrument>> previous_;
public:
    RegistryGuard() {
        std::lock_guard<std::mutex> lock(registry_.mutex_);
        previous_=registry_.instruments_; registry_.instruments_.clear();
        // Deliberately empty: explicit-context rendering must never consult registry.
    }
    ~RegistryGuard() {
        std::lock_guard<std::mutex> lock(registry_.mutex_);
        registry_.instruments_=std::move(previous_);
    }
};
class ContextCsvTest : public ::testing::Test {
protected:
    RegistryGuard registry_;
    std::filesystem::path dir_,output_;
    struct stat owned_{};
    StrategyPositionsMap rows_{{"ALPHA",{{"SYN",position("SYN",125000000)}}}};
    std::unordered_map<std::string,double> prices_{{"SYN",20.0}};
    void SetUp() override {
        std::string pattern="/tmp/algolens-desk-csv-XXXXXX";
        char* owned=::mkdtemp(pattern.data());
        ASSERT_NE(owned,nullptr); dir_=owned;
        ASSERT_EQ(::lstat(dir_.c_str(),&owned_),0);
        output_=dir_/"2026-09-26_positions.csv";
    }
    void TearDown() override {
        struct stat current{};
        if (::lstat(dir_.c_str(),&current)!=0 || !S_ISDIR(current.st_mode) ||
            current.st_dev!=owned_.st_dev || current.st_ino!=owned_.st_ino) {
            ADD_FAILURE()<<"owned CSV directory identity changed"; return;
        }
        // Only this test's named file and directory: never recursive cleanup.
        std::filesystem::remove(output_);
        EXPECT_TRUE(std::filesystem::remove(dir_));
    }
    void refusal_preserves_file(const StrategyPositionsMap& rows,
            const std::unordered_map<std::string,double>& prices,
            const DeskCsvInstruments& instruments) {
        std::ofstream(output_)<<"existing-reviewable-file\n";
        CSVExporter exporter(dir_.string());
        const auto result=exporter.export_desk_positions(day(),rows,prices,20000,10000,500,instruments);
        EXPECT_TRUE(result.is_error());
        if (result.is_error()) EXPECT_EQ(result.error()->code(),ErrorCode::INVALID_DATA);
        EXPECT_EQ(read(output_),"existing-reviewable-file\n");
    }
};

TEST_F(ContextCsvTest, ExplicitEquityContextRendersWithoutAnyRegistryInstrument) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_desk_positions(day(),rows_,prices_,20000,10000,500,
        DeskCsvInstruments{{"SYN",{AssetType::EQUITY,1.0}}});
    ASSERT_TRUE(result.is_ok()); const auto output=lines(read(output_));
    ASSERT_EQ(output.size(),4U);
    EXPECT_EQ(output[3],"Alpha,SYN,1.25,20,25,0.25,0.125");
    EXPECT_FALSE(InstrumentRegistry::instance().get_instrument("SYN"));
}
TEST_F(ContextCsvTest, ExplicitFuturesContextUsesActualMultiplierWithoutRegistry) {
    rows_.at("ALPHA").at("SYN").quantity=Quantity::from_raw(200000000);
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_desk_positions(day(),rows_,prices_,20000,10000,500,
        DeskCsvInstruments{{"SYN",{AssetType::FUTURE,50.0}}});
    ASSERT_TRUE(result.is_ok()); const auto output=lines(read(output_));
    ASSERT_EQ(output.size(),4U); EXPECT_EQ(output[3],"Alpha,SYN,2,20,2000,20,10");
}
TEST_F(ContextCsvTest, ExplicitZeroClosureStillRendersActualSnapshotRow) {
    rows_.at("ALPHA").at("SYN").quantity=Quantity::from_raw(0);
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_desk_positions(day(),rows_,prices_,20000,10000,500,
        DeskCsvInstruments{{"SYN",{AssetType::EQUITY,1.0}}});
    ASSERT_TRUE(result.is_ok()); const auto output=lines(read(output_));
    ASSERT_EQ(output.size(),4U); EXPECT_EQ(output[3],"Alpha,SYN,0,20,0,0,0");
}
TEST_F(ContextCsvTest, EmptyExplicitSnapshotNeedsNoInstrumentDefaults) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_desk_positions(day(),StrategyPositionsMap{}, {},20000,10000,500,{});
    ASSERT_TRUE(result.is_ok()); const auto output=lines(read(output_));
    ASSERT_EQ(output.size(),3U); EXPECT_EQ(output[1],"# Model columns: absent");
}
TEST_F(ContextCsvTest, MissingContextRefusesWithoutReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,{});
}
TEST_F(ContextCsvTest, FractionalFuturesContextRefusesWithoutReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,{{"SYN",{AssetType::FUTURE,50}}});
}
TEST_F(ContextCsvTest, UnsupportedAssetTypeRefusesWithoutReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,{{"SYN",{AssetType::NONE,1}}});
}
TEST_F(ContextCsvTest, ZeroMultiplierRefusesWithoutReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,{{"SYN",{AssetType::FUTURE,0}}});
}
TEST_F(ContextCsvTest, NonfiniteMultiplierRefusesWithoutReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,{{"SYN",{AssetType::FUTURE,std::numeric_limits<double>::infinity()}}});
}
TEST_F(ContextCsvTest, EquityMultiplierMustRemainActualShareUnit) {
    refusal_preserves_file(rows_,prices_,{{"SYN",{AssetType::EQUITY,50}}});
}
TEST_F(ContextCsvTest, ExtraneousInstrumentContextCannotExpandScope) {
    refusal_preserves_file(rows_,prices_,{{"SYN",{AssetType::EQUITY,1}}, {"FOREIGN",{AssetType::EQUITY,1}}});
}
TEST_F(ContextCsvTest, ExtraneousPricesCannotExpandScope) {
    auto extra=prices_;extra["FOREIGN"]=20;
    refusal_preserves_file(rows_,extra,{{"SYN",{AssetType::EQUITY,1}}});
}
TEST_F(ContextCsvTest, OldAbsentMapModeDoesNotConsumeOrInstallExplicitContext) {
    CSVExporter exporter(dir_.string());
    const auto refused=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500);
    EXPECT_TRUE(refused.is_error()); EXPECT_FALSE(std::filesystem::exists(output_));
}
} // namespace
#endif
