#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/backtest/csv_bar_source.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

std::filesystem::path find_repo_file(const std::string& relative) {
    namespace fs = std::filesystem;
    fs::path dir = fs::current_path();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (fs::exists(dir / relative)) return dir / relative;
        dir = dir.parent_path();
    }
    return {};
}

DataLoadConfig make_config(std::vector<std::string> symbols = {"ES", "NQ"}, size_t batch_size = 5) {
    DataLoadConfig c;
    c.symbols = std::move(symbols);
    c.start_date = std::chrono::system_clock::now() - std::chrono::hours(24 * 365);
    c.end_date = std::chrono::system_clock::now();
    c.asset_class = AssetClass::FUTURES;
    c.data_freq = DataFrequency::DAILY;
    c.data_type = "ohlcv";
    c.batch_size = batch_size;
    return c;
}

Bar make_bar(const std::string& symbol, double close, std::chrono::system_clock::time_point ts) {
    Bar bar;
    bar.symbol = symbol;
    bar.timestamp = ts;
    bar.open = Decimal(close);
    bar.high = Decimal(close);
    bar.low = Decimal(close);
    bar.close = Decimal(close);
    bar.volume = 1000.0;
    return bar;
}

}  // namespace

class BacktestDataLoaderTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        db_ = std::make_shared<MockPostgresDatabase>("mock://testdb");
    }
    std::shared_ptr<MockPostgresDatabase> db_;
};

TEST_F(BacktestDataLoaderTest, NullDatabaseLoadFailsWithConnectionError) {
    BacktestDataLoader loader(nullptr);
    auto result = loader.load_market_data(make_config());
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::CONNECTION_ERROR);
}

TEST_F(BacktestDataLoaderTest, EmptySymbolsListReturnsInvalidArgument) {
    BacktestDataLoader loader(db_);
    auto result = loader.load_market_data(make_config({}));
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(BacktestDataLoaderTest, AutoConnectsWhenDisconnected) {
    BacktestDataLoader loader(db_);
    ASSERT_FALSE(db_->is_connected());
    auto result = loader.load_market_data(make_config());
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_TRUE(db_->is_connected());
    EXPECT_FALSE(result.value().empty());
}

TEST_F(BacktestDataLoaderTest, AlreadyConnectedSkipsConnectCall) {
    db_->connect();
    BacktestDataLoader loader(db_);
    auto result = loader.load_market_data(make_config());
    ASSERT_TRUE(result.is_ok());
}

TEST_F(BacktestDataLoaderTest, LoadMarketDataBatchesSymbolsByConfiguredSize) {
    BacktestDataLoader loader(db_);
    // 7 symbols with batch_size=2 → 4 batches, all succeed against mock.
    auto result = loader.load_market_data(
        make_config({"S1", "S2", "S3", "S4", "S5", "S6", "S7"}, /*batch_size=*/2));
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value().empty());
}

TEST_F(BacktestDataLoaderTest, BatchSizeZeroFallsBackToDefaultOfFive) {
    BacktestDataLoader loader(db_);
    auto result = loader.load_market_data(make_config({"ES", "NQ"}, /*batch_size=*/0));
    ASSERT_TRUE(result.is_ok());
}

TEST_F(BacktestDataLoaderTest, GroupBarsByTimestampPartitionsCorrectly) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {
        make_bar("ES", 4000.0, t0),
        make_bar("NQ", 15000.0, t0),
        make_bar("ES", 4010.0, t0 + std::chrono::hours(24)),
    };
    auto grouped = loader.group_bars_by_timestamp(bars);
    EXPECT_EQ(grouped.size(), 2u);
    EXPECT_EQ(grouped.at(t0).size(), 2u);
    EXPECT_EQ(grouped.at(t0 + std::chrono::hours(24)).size(), 1u);
}

TEST_F(BacktestDataLoaderTest, ValidateDataQualityEmptyReturnsError) {
    BacktestDataLoader loader(db_);
    auto r = loader.validate_data_quality({});
    EXPECT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(BacktestDataLoaderTest, ValidateDataQualityFlatPricesReturnsError) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {
        make_bar("ES", 100.0, t0),
        make_bar("ES", 100.0, t0 + std::chrono::hours(24)),
        make_bar("ES", 100.0, t0 + std::chrono::hours(48)),
    };
    auto r = loader.validate_data_quality(bars);
    EXPECT_TRUE(r.is_error());
}

TEST_F(BacktestDataLoaderTest, ValidateDataQualityDetectsPriceMovement) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {
        make_bar("ES", 100.0, t0),
        make_bar("ES", 110.0, t0 + std::chrono::hours(24)),  // +10%
    };
    auto r = loader.validate_data_quality(bars);
    EXPECT_TRUE(r.is_ok());
}

TEST_F(BacktestDataLoaderTest, GetUniqueSymbolsReturnsSortedSet) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {
        make_bar("NQ", 1.0, t0), make_bar("ES", 1.0, t0),
        make_bar("ES", 1.0, t0), make_bar("CL", 1.0, t0),
    };
    auto syms = loader.get_unique_symbols(bars);
    ASSERT_EQ(syms.size(), 3u);
    EXPECT_EQ(syms[0], "CL");
    EXPECT_EQ(syms[1], "ES");
    EXPECT_EQ(syms[2], "NQ");
}

TEST_F(BacktestDataLoaderTest, GetDateRangeEmptyReturnsDefaultPair) {
    BacktestDataLoader loader(db_);
    auto [lo, hi] = loader.get_date_range({});
    EXPECT_EQ(lo, Timestamp{});
    EXPECT_EQ(hi, Timestamp{});
}

TEST_F(BacktestDataLoaderTest, GetDateRangeFindsMinAndMax) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    auto t1 = t0 + std::chrono::hours(24);
    auto t2 = t0 + std::chrono::hours(48);
    std::vector<Bar> bars = {make_bar("ES", 1.0, t1), make_bar("ES", 1.0, t0),
                              make_bar("ES", 1.0, t2)};
    auto [lo, hi] = loader.get_date_range(bars);
    EXPECT_EQ(lo, t0);
    EXPECT_EQ(hi, t2);
}

TEST_F(BacktestDataLoaderTest, PriceStatisticsForSymbol) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {
        make_bar("ES", 100.0, t0),
        make_bar("ES", 110.0, t0 + std::chrono::hours(24)),
        make_bar("NQ", 200.0, t0),  // different symbol — should be ignored
    };
    auto stats = loader.get_price_statistics(bars, "ES");
    EXPECT_DOUBLE_EQ(stats["min_price"], 100.0);
    EXPECT_DOUBLE_EQ(stats["max_price"], 110.0);
    EXPECT_NEAR(stats["price_range_pct"], 10.0, 1e-9);
}

TEST_F(BacktestDataLoaderTest, PriceStatisticsMissingSymbolReturnsZeros) {
    BacktestDataLoader loader(db_);
    auto t0 = std::chrono::system_clock::now();
    std::vector<Bar> bars = {make_bar("ES", 100.0, t0)};
    auto stats = loader.get_price_statistics(bars, "DOES_NOT_EXIST");
    EXPECT_DOUBLE_EQ(stats["min_price"], 0.0);
    EXPECT_DOUBLE_EQ(stats["max_price"], 0.0);
    EXPECT_DOUBLE_EQ(stats["price_range_pct"], 0.0);
}

TEST_F(BacktestDataLoaderTest, OptionsAssetClassRejectedUpfront) {
    // Options market data is not yet ingested by data-ngin.
    // The rejection must happen early with a clear error message,
    // not silently query an unknown_data schema that does not exist.
    BacktestDataLoader loader(db_);
    auto config = make_config();
    config.asset_class = AssetClass::OPTIONS;  // Change to OPTIONS
    auto result = loader.load_market_data(config);
    ASSERT_TRUE(result.is_error()) << "Options should be rejected";
    EXPECT_EQ(result.error()->code(), ErrorCode::MARKET_DATA_ERROR);
    std::string msg = result.error()->what();
    EXPECT_NE(msg.find("data-ngin"), std::string::npos)
        << "Error message should mention data-ngin";
    EXPECT_NE(msg.find("not yet ingested"), std::string::npos)
        << "Error message should clarify options are not ingested";
}

TEST_F(BacktestDataLoaderTest, OptionsAssetClassErrorIncludesRoadmapReference) {
    // Ensure the OPTIONS rejection error message includes the roadmap reference (data-ngin#39).
    // This provides users with actionable next steps.
    BacktestDataLoader loader(db_);
    auto config = make_config();
    config.asset_class = AssetClass::OPTIONS;
    auto result = loader.load_market_data(config);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::MARKET_DATA_ERROR);
    std::string msg = result.error()->what();

    // Verify all expected clauses are present in the error message
    EXPECT_NE(msg.find("Options"), std::string::npos)
        << "Error should mention 'Options'";
    EXPECT_NE(msg.find("market data"), std::string::npos)
        << "Error should mention 'market data'";
    EXPECT_NE(msg.find("data-ngin#39"), std::string::npos)
        << "Error should reference data-ngin#39 roadmap";
}

// ---- require_all_symbols / require_aligned_calendar (default off) --------------------
TEST_F(BacktestDataLoaderTest, RequireAllSymbolsDefaultsFalseAndKeepsPartialLoads) {
    BacktestDataLoader loader(db_);
    // The mock always serves AAPL and GOOG whatever is requested; XYZ never comes back.
    auto cfg = make_config({"AAPL", "GOOG", "XYZ"});
    EXPECT_FALSE(cfg.require_all_symbols);
    EXPECT_FALSE(cfg.require_aligned_calendar);
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_FALSE(result.value().empty());
}

TEST_F(BacktestDataLoaderTest, RequireAllSymbolsAbortsWithSymbolList) {
    BacktestDataLoader loader(db_);
    auto cfg = make_config({"AAPL", "GOOG", "XYZ"});
    cfg.require_all_symbols = true;
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::MARKET_DATA_ERROR);
    std::string msg = result.error()->what();
    EXPECT_NE(msg.find("XYZ"), std::string::npos) << msg;
    EXPECT_EQ(msg.find("AAPL"), std::string::npos) << "served symbols are not reported: " << msg;
}

TEST_F(BacktestDataLoaderTest, RequireAllSymbolsPassesWhenEveryoneHasBars) {
    BacktestDataLoader loader(db_);
    auto cfg = make_config({"AAPL", "GOOG"});
    cfg.require_all_symbols = true;
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
}

namespace {
std::chrono::system_clock::time_point utc_day(int y, int m, int d) {
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}
}  // namespace

TEST_F(BacktestDataLoaderTest, AlignedCalendarAcceptsACompleteGrid) {
    BacktestDataLoader loader(db_);
    std::vector<Bar> bars;
    for (int d = 1; d <= 3; ++d) {
        bars.push_back(make_bar("A", 100.0 + d, utc_day(2024, 1, d)));
        bars.push_back(make_bar("B", 50.0 + d, utc_day(2024, 1, d)));
    }
    EXPECT_TRUE(loader.validate_calendar_alignment(bars).is_ok());
}

TEST_F(BacktestDataLoaderTest, AlignedCalendarRejectsGap) {
    BacktestDataLoader loader(db_);
    std::vector<Bar> bars = {
        make_bar("A", 101.0, utc_day(2024, 1, 1)), make_bar("B", 51.0, utc_day(2024, 1, 1)),
        make_bar("A", 102.0, utc_day(2024, 1, 2)),  // B has no bar on the 2nd
        make_bar("A", 103.0, utc_day(2024, 1, 3)), make_bar("B", 53.0, utc_day(2024, 1, 3)),
    };
    auto r = loader.validate_calendar_alignment(bars);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::MARKET_DATA_ERROR);
    std::string msg = r.error()->what();
    EXPECT_NE(msg.find("B missing 2024-01-02"), std::string::npos) << msg;
}

TEST_F(BacktestDataLoaderTest, AlignedCalendarRejectsTwoBarsOneUtcDate) {
    BacktestDataLoader loader(db_);
    std::vector<Bar> bars = {
        make_bar("A", 101.0, utc_day(2024, 1, 1)),
        make_bar("A", 101.5, utc_day(2024, 1, 1) + std::chrono::hours(8)),  // same UTC day
    };
    auto r = loader.validate_calendar_alignment(bars);
    ASSERT_TRUE(r.is_error());
    EXPECT_NE(std::string(r.error()->what()).find("two bars on 2024-01-01"), std::string::npos);
}

TEST_F(BacktestDataLoaderTest, AlignedCalendarAllowsLateInceptionAndEarlyEnd) {
    BacktestDataLoader loader(db_);
    std::vector<Bar> bars;
    for (int d = 1; d <= 5; ++d) bars.push_back(make_bar("A", 100.0 + d, utc_day(2024, 1, d)));
    // B lists on the 3rd and stops after the 4th: no gaps inside its own range.
    bars.push_back(make_bar("B", 53.0, utc_day(2024, 1, 3)));
    bars.push_back(make_bar("B", 54.0, utc_day(2024, 1, 4)));
    EXPECT_TRUE(loader.validate_calendar_alignment(bars).is_ok());
}

TEST_F(BacktestDataLoaderTest, RequireAlignedCalendarIsEnforcedByLoadMarketData) {
    // The mock serves AAPL and GOOG on two DIFFERENT timestamps (now, now+1h) with one bar
    // each, so each symbol's own range is a single day: aligned. The flag must not reject
    // that, and must be off by default.
    BacktestDataLoader loader(db_);
    auto cfg = make_config({"AAPL", "GOOG"});
    cfg.require_aligned_calendar = true;
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
}

// ---- DataSource::CSV routing and structural isolation from the live path -------------
namespace {

const char* kTwoDayCsv =
    "date,open,high,low,close,volume,div_cash,split_factor\n"
    "2024-01-02,10,11,9,10,100,0,1\n"
    "2024-01-03,10,12,9,11,100,0,1\n";

class CsvSourceFixture {
public:
    CsvSourceFixture() {
        dir_ = std::filesystem::temp_directory_path() /
               ("csv_source_" + std::to_string(::getpid()) + "_" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(dir_);
    }
    ~CsvSourceFixture() { std::filesystem::remove_all(dir_); }
    void write(const std::string& symbol, const std::string& content = kTwoDayCsv) {
        std::ofstream out(csv_bar_path(dir_, symbol));
        out << content;
    }
    DataLoadConfig config(std::vector<std::string> symbols) {
        auto c = make_config(std::move(symbols));
        c.asset_class = AssetClass::EQUITIES;
        c.data_source = DataSource::CSV;
        c.csv_bar_dir = dir_.string();
        c.start_date = std::chrono::system_clock::from_time_t(0);
        return c;
    }
    std::filesystem::path dir_;
};

}  // namespace

TEST_F(BacktestDataLoaderTest, DefaultDataLoadConfigUsesRealSource) {
    DataLoadConfig c;
    EXPECT_EQ(c.data_source, DataSource::REAL);
    EXPECT_TRUE(c.csv_bar_dir.empty());
}

TEST_F(BacktestDataLoaderTest, CsvSourceLoadsWithNullDatabase) {
    CsvSourceFixture fx;
    fx.write("AAA");
    fx.write("BBB");
    BacktestDataLoader loader(nullptr);  // no database at all
    auto result = loader.load_market_data(fx.config({"AAA", "BBB"}));
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_EQ(result.value().size(), 4u);
    // ORDER BY time, symbol -- the same contract as the SQL path.
    EXPECT_EQ(result.value()[0].symbol, "AAA");
    EXPECT_EQ(result.value()[1].symbol, "BBB");
    EXPECT_EQ(result.value()[2].symbol, "AAA");
}

TEST_F(BacktestDataLoaderTest, CsvSourceNeverFallsBackToDatabase) {
    CsvSourceFixture fx;
    fx.write("AAA");  // no file for AAPL, which the mock database WOULD serve
    db_->connect();
    BacktestDataLoader loader(db_);
    auto cfg = fx.config({"AAA", "AAPL"});
    cfg.batch_size = 1;
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    for (const auto& bar : result.value()) {
        EXPECT_EQ(bar.symbol, "AAA") << "AAPL must not appear: no CSV, and no DB fallback";
    }
    cfg.require_all_symbols = true;
    auto strict = loader.load_market_data(cfg);
    ASSERT_TRUE(strict.is_error());
    EXPECT_NE(std::string(strict.error()->what()).find("AAPL"), std::string::npos);
}

TEST_F(BacktestDataLoaderTest, CsvBatchSkipsOnlyTheSymbolWithoutBars) {
    // One symbol without a file in a batch of three: its partners still load, as on the
    // database path, where a symbol with no rows is simply absent from the batch result.
    // Failing the whole batch used to drop BIL with a late-inception DBMF.
    CsvSourceFixture fx;
    fx.write("AAA");
    fx.write("CCC");  // no file for BBB
    BacktestDataLoader loader(nullptr);
    auto cfg = fx.config({"AAA", "BBB", "CCC"});
    cfg.batch_size = 5;  // all three in one batch
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    ASSERT_EQ(result.value().size(), 4u) << "AAA and CCC, two bars each";
    for (const auto& bar : result.value()) EXPECT_NE(bar.symbol, "BBB");

    cfg.require_all_symbols = true;  // strictness still refuses the absent symbol
    auto strict = loader.load_market_data(cfg);
    ASSERT_TRUE(strict.is_error());
    EXPECT_NE(std::string(strict.error()->what()).find("BBB"), std::string::npos);
}

TEST_F(BacktestDataLoaderTest, CsvBatchStillFailsOnAMalformedFile) {
    // A file that exists but violates the contract is a data-integrity error, not an
    // absent symbol: the batch fails and nothing from it is used.
    CsvSourceFixture fx;
    fx.write("AAA");
    fx.write("BAD", "date,open,high,low,close,volume,div_cash,split_factor\n"
                    "2024-01-03,10,12,9,11,100,0,1\n"
                    "2024-01-02,10,11,9,10,100,0,1\n");  // dates out of order
    BacktestDataLoader loader(nullptr);
    auto cfg = fx.config({"AAA", "BAD"});
    cfg.batch_size = 5;
    auto result = loader.load_market_data(cfg);
    const bool dropped = result.is_error() || result.value().empty();
    EXPECT_TRUE(dropped) << "a malformed file must not be skipped as if it were absent";
}

TEST_F(BacktestDataLoaderTest, RealSourceNeverReadsCsv) {
    CsvSourceFixture fx;
    fx.write("AAPL",
             "date,open,high,low,close,volume,div_cash,split_factor\n"
             "2024-01-02,1,1,1,1,1,0,1\n");  // poisoned close of 1.0
    BacktestDataLoader loader(db_);
    auto cfg = make_config({"AAPL", "GOOG"});
    cfg.asset_class = AssetClass::EQUITIES;
    cfg.csv_bar_dir = fx.dir_.string();  // set, but data_source stays REAL
    auto result = loader.load_market_data(cfg);
    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    for (const auto& bar : result.value()) {
        EXPECT_GT(bar.close.as_double(), 100.0) << "closes come from the mock database";
    }
}

TEST_F(BacktestDataLoaderTest, CsvSourceRequiresDirAndEquities) {
    CsvSourceFixture fx;
    fx.write("AAA");
    BacktestDataLoader loader(nullptr);
    auto no_dir = fx.config({"AAA"});
    no_dir.csv_bar_dir.clear();
    auto r1 = loader.load_market_data(no_dir);
    ASSERT_TRUE(r1.is_error());
    EXPECT_EQ(r1.error()->code(), ErrorCode::INVALID_ARGUMENT);

    auto futures = fx.config({"AAA"});
    futures.asset_class = AssetClass::FUTURES;
    auto r2 = loader.load_market_data(futures);
    ASSERT_TRUE(r2.is_error());
    EXPECT_EQ(r2.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(BacktestDataLoaderTest, LivePathHasNoCsvVocabulary) {
    // (a) Every asset class the live path can name resolves to a *_data schema; there is
    //     no value that reaches CSV bars through the functions the live apps use.
    for (auto ac : {AssetClass::FUTURES, AssetClass::EQUITIES, AssetClass::OPTIONS,
                    AssetClass::FIXED_INCOME, AssetClass::CURRENCIES, AssetClass::COMMODITIES,
                    AssetClass::CRYPTO}) {
        const std::string schema = get_schema_name(ac);
        EXPECT_NE(schema.find("_data"), std::string::npos) << schema;
        EXPECT_EQ(schema.find("csv"), std::string::npos) << schema;
    }
    // (b) The live sources never mention the CSV source at all.
    const auto src_dir = find_repo_file("src");
    if (src_dir.empty()) GTEST_SKIP() << "repository root not reachable from the test cwd";
    const auto root = src_dir.parent_path();
    std::vector<std::filesystem::path> files = {root / "src" / "data" / "postgres_database.cpp"};
    for (const auto& dir : {root / "src" / "live", root / "apps" / "strategies"}) {
        if (!std::filesystem::exists(dir)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().extension() == ".cpp") files.push_back(entry.path());
        }
    }
    ASSERT_GT(files.size(), 1u);
    for (const auto& file : files) {
        std::ifstream in(file);
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_EQ(src.find("csv_bar_source"), std::string::npos) << file;
        EXPECT_EQ(src.find("DataSource::CSV"), std::string::npos) << file;
    }
}
