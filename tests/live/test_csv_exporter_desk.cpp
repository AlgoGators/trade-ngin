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
        FuturesSpec spec;
        spec.root_symbol="ES"; spec.exchange="CME"; spec.currency="USD";
        spec.multiplier=50.0; spec.tick_size=0.25; spec.weight=1.0;
        registry_.instruments_["ES"]=std::make_shared<FuturesInstrument>("ES",spec);
    }
    ~RegistryGuard() {
        std::lock_guard<std::mutex> lock(registry_.mutex_);
        registry_.instruments_=std::move(previous_);
    }
};
class DeskCsvTest : public ::testing::Test {
protected:
    RegistryGuard registry_;
    std::filesystem::path dir_,output_;
    struct stat owned_{};
    StrategyPositionsMap rows_{{"ALPHA",{{"ES.v.0",position("ES.v.0",125000000)}}}};
    std::unordered_map<std::string,double> prices_{{"ES.v.0",20.0}};
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
            double equity=20000.0,double gross=10000.0,double net=500.0) {
        std::ofstream(output_)<<"existing-reviewable-file\n";
        CSVExporter exporter(dir_.string());
        const auto result=exporter.export_current_positions(day(),rows,prices,equity,gross,net);
        EXPECT_TRUE(result.is_error());
        if (result.is_error()) EXPECT_EQ(result.error()->code(),ErrorCode::INVALID_DATA);
        EXPECT_EQ(read(output_),"existing-reviewable-file\n");
    }
};
const std::string summary="# Portfolio Value: 20000.00, Gross Notional: 10000.00, Net Notional: 500.00, Date: 2026-09-26\n";
const std::string desk_header="# Model columns: absent\nstrategy,symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_portfolio_value\n";
const std::string legacy_header="strategy,symbol,quantity,market_price,notional,pct_of_gross_notional,pct_of_portfolio_value,forecast,volatility,ema_8,ema_32,ema_64,ema_256\n";

TEST_F(DeskCsvTest, AbsentStrategyMapHasOnlyKnownColumnsAndExactRow) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500);
    ASSERT_TRUE(result.is_ok()); EXPECT_EQ(result.value(),output_.string());
    EXPECT_EQ(read(output_),summary+desk_header+"Alpha,ES.v.0,1.25,20,1250,12.5,6.25\n");
}
TEST_F(DeskCsvTest, SameSymbolOwnersStaySeparateAndSignedFractionalTextIsExact) {
    rows_["ZETA"]={{"ES.v.0",position("ES.v.0",-1)}};
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500);
    ASSERT_TRUE(result.is_ok()); const auto text=lines(read(output_));
    ASSERT_EQ(text.size(),5U);
    EXPECT_EQ(text[3],"Alpha,ES.v.0,1.25,20,1250,12.5,6.25");
    EXPECT_EQ(text[4].find("Zeta,ES.v.0,-0.00000001,20,"),0U);
    EXPECT_EQ(rows_.at("ZETA").at("ES.v.0").quantity.to_string(),"-0.00000001");
}
TEST_F(DeskCsvTest, ExplicitZeroClosureIsKeptWithoutInventingUniverseRows) {
    rows_.at("ALPHA").at("ES.v.0").quantity=Quantity::from_raw(0);
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(read(output_),summary+desk_header+"Alpha,ES.v.0,0,20,0,0,0\n");
}
TEST_F(DeskCsvTest, EmptySnapshotHasSummaryAndHeadersOnly) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),StrategyPositionsMap{}, {},20000,10000,500);
    ASSERT_TRUE(result.is_ok()); EXPECT_EQ(read(output_),summary+desk_header);
}
TEST_F(DeskCsvTest, FiniteZeroDenominatorsKeepZeroPercentages) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,0,0,500);
    ASSERT_TRUE(result.is_ok()); const auto text=lines(read(output_));
    ASSERT_EQ(text.size(),4U); EXPECT_EQ(text[3],"Alpha,ES.v.0,1.25,20,1250,0,0");
}
TEST_F(DeskCsvTest, ExplicitEmptyMapRetainsCompleteLegacyBytes) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500,StrategyInstancesMap{});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(read(output_),summary+legacy_header+
        "Alpha,ES.v.0,1.25,20.00,1250.00,12.50,6.25,0.00,0.000000,0.000000,0.000000,0.000000,0.000000\n");
}
TEST_F(DeskCsvTest, ExplicitBraceMapRemainsUnambiguousAndPreservesLegacyBytes) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500,{});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(read(output_),summary+legacy_header+
        "Alpha,ES.v.0,1.25,20.00,1250.00,12.50,6.25,0.00,0.000000,0.000000,0.000000,0.000000,0.000000\n");
}
TEST_F(DeskCsvTest, ExplicitNullInstanceMapStillSelectsLegacyMode) {
    CSVExporter exporter(dir_.string());
    const auto result=exporter.export_current_positions(day(),rows_,prices_,20000,10000,500,StrategyInstancesMap{{"ALPHA",nullptr}},true);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(read(output_),summary+legacy_header+
        "Alpha,ES.v.0,1.25,20.00,1250.00,12.50,6.25,0.00,0.000000,0.000000,0.000000,0.000000,0.000000\n");
}
TEST_F(DeskCsvTest, MissingExplicitPriceRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,{});
}
TEST_F(DeskCsvTest, ZeroPriceRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,{{"ES.v.0",0}});
}
TEST_F(DeskCsvTest, NegativePriceRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,{{"ES.v.0",-20}});
}
TEST_F(DeskCsvTest, NonfinitePriceRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,{{"ES.v.0",std::numeric_limits<double>::infinity()}});
}
TEST_F(DeskCsvTest, NanPriceRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,{{"ES.v.0",std::numeric_limits<double>::quiet_NaN()}});
}
TEST_F(DeskCsvTest, MissingInstrumentRefusesBeforeReplacingExistingFile) {
    StrategyPositionsMap rows{{"ALPHA",{{"UNREGISTERED",position("UNREGISTERED",125000000)}}}};
    refusal_preserves_file(rows,{{"UNREGISTERED",20}});
}
TEST_F(DeskCsvTest, NonfinitePortfolioSummaryRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,std::numeric_limits<double>::infinity());
}
TEST_F(DeskCsvTest, NegativeGrossRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,prices_,20000,-10000);
}
TEST_F(DeskCsvTest, OverflowingNotionalRefusesBeforeReplacingExistingFile) {
    refusal_preserves_file(rows_,{{"ES.v.0",std::numeric_limits<double>::max()}});
}
TEST_F(DeskCsvTest, ZeroClosureStillRequiresItsExplicitPrice) {
    rows_.at("ALPHA").at("ES.v.0").quantity=Quantity::from_raw(0);
    refusal_preserves_file(rows_,{});
}
} // namespace
#endif
