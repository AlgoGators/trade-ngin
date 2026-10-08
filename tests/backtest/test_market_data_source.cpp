#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/data/market_data_source.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;

namespace {

/// In-memory MarketDataSource used to drive a backtest with no database.
class InMemoryDataSource : public MarketDataSource {
public:
    explicit InMemoryDataSource(std::vector<Bar> bars) : bars_(std::move(bars)) {}

    Result<std::vector<Bar>> load_bars(const std::vector<std::string>& symbols,
                                       const Timestamp& start_date, const Timestamp& end_date,
                                       AssetClass asset_class, DataFrequency freq) override {
        (void)asset_class;
        (void)freq;
        ++load_calls;

        std::vector<Bar> selected;
        for (const auto& bar : bars_) {
            bool wanted = std::find(symbols.begin(), symbols.end(), bar.symbol) != symbols.end();
            if (wanted && bar.timestamp >= start_date && bar.timestamp <= end_date) {
                selected.push_back(bar);
            }
        }

        return Result<std::vector<Bar>>(selected);
    }

    Result<std::vector<std::string>> get_symbols(AssetClass asset_class) override {
        (void)asset_class;
        return Result<std::vector<std::string>>(std::vector<std::string>{"AAA", "BBB"});
    }

    int load_calls = 0;

private:
    std::vector<Bar> bars_;
};

/// A source that always fails, to check errors propagate through the loader.
class FailingDataSource : public MarketDataSource {
public:
    Result<std::vector<Bar>> load_bars(const std::vector<std::string>&, const Timestamp&,
                                       const Timestamp&, AssetClass, DataFrequency) override {
        return make_error<std::vector<Bar>>(ErrorCode::MARKET_DATA_ERROR, "provider unavailable",
                                            "FailingDataSource");
    }

    Result<std::vector<std::string>> get_symbols(AssetClass) override {
        return make_error<std::vector<std::string>>(ErrorCode::MARKET_DATA_ERROR, "unavailable",
                                                    "FailingDataSource");
    }
};

Timestamp day(int index) {
    return std::chrono::system_clock::time_point{} + std::chrono::hours(24 * index);
}

/// Two symbols, `days` bars each, with enough movement to pass validation.
std::vector<Bar> make_bars(int days) {
    std::vector<Bar> bars;
    for (int i = 0; i < days; ++i) {
        double drift = 1.0 + 0.01 * i;
        bars.emplace_back(day(i), 100.0 * drift, 101.0 * drift, 99.0 * drift, 100.5 * drift,
                          1000.0, "AAA");
        bars.emplace_back(day(i), 200.0 * drift, 202.0 * drift, 198.0 * drift, 201.0 * drift,
                          2000.0, "BBB");
    }
    return bars;
}

DataLoadConfig make_config(int days) {
    DataLoadConfig config;
    config.symbols = {"AAA", "BBB"};
    config.start_date = day(0);
    config.end_date = day(days);
    config.asset_class = AssetClass::EQUITIES;
    config.data_freq = DataFrequency::DAILY;
    return config;
}

}  // namespace

TEST(MarketDataSourceTest, LoaderReadsFromInjectedSource) {
    constexpr int kDays = 10;
    auto source = std::make_shared<InMemoryDataSource>(make_bars(kDays));
    BacktestDataLoader loader(source);

    auto result = loader.load_market_data(make_config(kDays));

    ASSERT_TRUE(result.is_ok()) << result.error()->what();
    EXPECT_EQ(result.value().size(), kDays * 2);
    EXPECT_EQ(source->load_calls, 1);
}

TEST(MarketDataSourceTest, GroupsBarsByTimestamp) {
    constexpr int kDays = 10;
    auto source = std::make_shared<InMemoryDataSource>(make_bars(kDays));
    BacktestDataLoader loader(source);

    auto result = loader.load_market_data(make_config(kDays));
    ASSERT_TRUE(result.is_ok());

    auto grouped = loader.group_bars_by_timestamp(result.value());

    EXPECT_EQ(grouped.size(), static_cast<size_t>(kDays));
    for (const auto& [timestamp, bars] : grouped) {
        EXPECT_EQ(bars.size(), 2u) << "expected one bar per symbol per day";
    }
}

TEST(MarketDataSourceTest, ValidatesQualityOfSourcedBars) {
    constexpr int kDays = 10;
    auto source = std::make_shared<InMemoryDataSource>(make_bars(kDays));
    BacktestDataLoader loader(source);

    auto result = loader.load_market_data(make_config(kDays));
    ASSERT_TRUE(result.is_ok());

    EXPECT_TRUE(loader.validate_data_quality(result.value()).is_ok());
    EXPECT_EQ(loader.get_unique_symbols(result.value()).size(), 2u);
}

TEST(MarketDataSourceTest, RespectsRequestedDateRange) {
    constexpr int kDays = 10;
    auto source = std::make_shared<InMemoryDataSource>(make_bars(kDays));
    BacktestDataLoader loader(source);

    auto config = make_config(kDays);
    config.end_date = day(4);  // Days 0..4 inclusive.

    auto result = loader.load_market_data(config);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().size(), 5u * 2u);
}

TEST(MarketDataSourceTest, RequestingOneSymbolExcludesOthers) {
    constexpr int kDays = 10;
    auto source = std::make_shared<InMemoryDataSource>(make_bars(kDays));
    BacktestDataLoader loader(source);

    auto config = make_config(kDays);
    config.symbols = {"AAA"};

    auto result = loader.load_market_data(config);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().size(), static_cast<size_t>(kDays));
    for (const auto& bar : result.value()) {
        EXPECT_EQ(bar.symbol, "AAA");
    }
}

TEST(MarketDataSourceTest, PropagatesSourceErrors) {
    BacktestDataLoader loader(std::make_shared<FailingDataSource>());

    auto result = loader.load_market_data(make_config(10));

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::MARKET_DATA_ERROR);
}

TEST(MarketDataSourceTest, RejectsEmptySymbolList) {
    auto source = std::make_shared<InMemoryDataSource>(make_bars(10));
    BacktestDataLoader loader(source);

    auto config = make_config(10);
    config.symbols.clear();

    auto result = loader.load_market_data(config);

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::INVALID_ARGUMENT);
    EXPECT_EQ(source->load_calls, 0) << "should not reach the source with no symbols";
}

TEST(MarketDataSourceTest, ReportsErrorWhenSourceReturnsNothing) {
    auto source = std::make_shared<InMemoryDataSource>(std::vector<Bar>{});
    BacktestDataLoader loader(source);

    auto result = loader.load_market_data(make_config(10));

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::MARKET_DATA_ERROR);
}

TEST(MarketDataSourceTest, ReportsErrorWhenNoSourceConfigured) {
    BacktestDataLoader loader(std::shared_ptr<MarketDataSource>{});

    auto result = loader.load_market_data(make_config(10));

    ASSERT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), ErrorCode::NOT_INITIALIZED);
}

TEST(MarketDataSourceTest, ExposesSymbolsFromSource) {
    InMemoryDataSource source{make_bars(1)};

    auto result = source.get_symbols(AssetClass::EQUITIES);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().size(), 2u);
}
