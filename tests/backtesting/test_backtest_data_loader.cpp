#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/backtest/backtest_data_loader.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

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
