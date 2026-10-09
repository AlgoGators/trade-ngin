// Listing dates at the backtest's bar loader (data/listing_dates.hpp): a predecessor contract has
// no stored rows of its own, so the loader leaves it out of the query and hands it the rows stored
// under its listed contract's symbol. With nothing declared the loader asks for and returns exactly
// what it did before. A declared vendor relabel (instrument_id_relabels) is read here too, before
// the predecessor's copy is made, so every consumer of the backtest sees one contract.

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <arrow/api.h>

#include "../core/test_base.hpp"
#include "../data/test_db_utils.hpp"
#include "trade_ngin/backtest/backtest_data_loader.hpp"
#include "trade_ngin/data/listing_dates.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

struct StoredBar {
    std::string symbol;
    std::chrono::sys_days day;
    double close;
    std::string instrument_id;
};

// A database whose bar table is `rows`: a query returns the rows of the symbols asked for, with
// the instrument_id column the futures loader joins.
class BarTableDatabase : public MockPostgresDatabase {
public:
    BarTableDatabase() : MockPostgresDatabase("mock://bars") { (void)connect(); }
    std::vector<StoredBar> rows;
    std::vector<std::vector<std::string>> asked;

    Result<std::shared_ptr<arrow::Table>> get_market_data(
        const std::vector<std::string>& symbols, const Timestamp&, const Timestamp&, AssetClass,
        DataFrequency, const std::string&) override {
        asked.push_back(symbols);
        arrow::TimestampBuilder time(arrow::timestamp(arrow::TimeUnit::SECOND), arrow::default_memory_pool());
        arrow::StringBuilder symbol, id;
        arrow::DoubleBuilder open, high, low, close, volume;
        for (const auto& row : rows) {
            if (std::find(symbols.begin(), symbols.end(), row.symbol) == symbols.end()) continue;
            ARROW_CHECK_OK(time.Append(std::chrono::duration_cast<std::chrono::seconds>(
                                           row.day.time_since_epoch()).count()));
            ARROW_CHECK_OK(symbol.Append(row.symbol));
            ARROW_CHECK_OK(open.Append(row.close));
            ARROW_CHECK_OK(high.Append(row.close + 1.0));
            ARROW_CHECK_OK(low.Append(row.close - 1.0));
            ARROW_CHECK_OK(close.Append(row.close));
            ARROW_CHECK_OK(volume.Append(1000.0));
            ARROW_CHECK_OK(id.Append(row.instrument_id));
        }
        std::shared_ptr<arrow::Array> a_time, a_symbol, a_open, a_high, a_low, a_close, a_volume, a_id;
        ARROW_CHECK_OK(time.Finish(&a_time));
        ARROW_CHECK_OK(symbol.Finish(&a_symbol));
        ARROW_CHECK_OK(open.Finish(&a_open));
        ARROW_CHECK_OK(high.Finish(&a_high));
        ARROW_CHECK_OK(low.Finish(&a_low));
        ARROW_CHECK_OK(close.Finish(&a_close));
        ARROW_CHECK_OK(volume.Finish(&a_volume));
        ARROW_CHECK_OK(id.Finish(&a_id));
        auto schema = arrow::schema(
            {arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
             arrow::field("symbol", arrow::utf8()), arrow::field("open", arrow::float64()),
             arrow::field("high", arrow::float64()), arrow::field("low", arrow::float64()),
             arrow::field("close", arrow::float64()), arrow::field("volume", arrow::float64()),
             arrow::field("instrument_id", arrow::utf8())});
        return Result<std::shared_ptr<arrow::Table>>(arrow::Table::Make(
            schema, {a_time, a_symbol, a_open, a_high, a_low, a_close, a_volume, a_id}));
    }
};

DataLoadConfig config_for(std::vector<std::string> symbols) {
    using namespace std::chrono_literals;
    DataLoadConfig c;
    c.symbols = std::move(symbols);
    c.start_date = std::chrono::sys_days{2026y / 1 / 1};
    c.end_date = std::chrono::sys_days{2026y / 12 / 31};
    c.asset_class = AssetClass::FUTURES;
    c.data_freq = DataFrequency::DAILY;
    c.data_type = "ohlcv";
    c.batch_size = 5;
    return c;
}

/// "symbol date-as-day-of-month close id" of every loaded bar of `symbol`, in the loader's order.
std::vector<std::string> view(const std::vector<Bar>& bars, const std::string& symbol) {
    std::vector<std::string> out;
    for (const auto& b : bars) {
        if (b.symbol != symbol) continue;
        const std::chrono::year_month_day d{std::chrono::floor<std::chrono::days>(b.timestamp)};
        out.push_back(std::to_string(static_cast<unsigned>(d.month())) + "-" +
                      std::to_string(static_cast<unsigned>(d.day())) + " " +
                      std::to_string(static_cast<int>(static_cast<double>(b.close))) + " " +
                      b.instrument_id);
    }
    return out;
}

}  // namespace

class ListingLoaderTest : public TestBase {
protected:
    void SetUp() override {
        TestBase::SetUp();
        reset();
        using namespace std::chrono_literals;
        db_ = std::make_shared<BarTableDatabase>();
        db_->rows = {{"MES.v.0", 2026y / 2 / 20, 6924.0, "42140878"},
                     {"MES.v.0", 2026y / 2 / 22, 6906.0, "42003800"},
                     {"MES.v.0", 2026y / 2 / 23, 6857.0, "42003800"},
                     {"ZN.v.0", 2026y / 2 / 20, 112.0, "42003800"},
                     {"ZN.v.0", 2026y / 2 / 23, 113.0, "42003800"}};
    }
    void TearDown() override {
        reset();
        TestBase::TearDown();
    }
    static void reset() {
        ListingDates::instance().clear();
        ListingDates::instance().set_relabels({});
    }
    std::shared_ptr<BarTableDatabase> db_;
};

TEST_F(ListingLoaderTest, WithNothingDeclaredTheLoaderAsksForAndReturnsTheStoredRows) {
    BacktestDataLoader loader(db_);
    const auto loaded = loader.load_market_data(config_for({"MES.v.0", "ZN.v.0"}));
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    ASSERT_EQ(db_->asked.size(), 1u);
    EXPECT_EQ(db_->asked[0], (std::vector<std::string>{"MES.v.0", "ZN.v.0"}));
    EXPECT_EQ(loaded.value().size(), 5u);
    EXPECT_EQ(view(loaded.value(), "MES.v.0"),
              (std::vector<std::string>{"2-20 6924 42140878", "2-22 6906 42003800", "2-23 6857 42003800"}));
}

TEST_F(ListingLoaderTest, APredecessorIsNotQueriedAndReadsTheRowsStoredUnderItsListedContract) {
    ListingDates::instance().set({{"MES", "ES", "2026-02-22", 10.0}});
    BacktestDataLoader loader(db_);
    const auto loaded = loader.load_market_data(config_for({"MES.v.0", "ZN.v.0", "ES.v.0"}));
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    ASSERT_EQ(db_->asked.size(), 1u);
    EXPECT_EQ(db_->asked[0], (std::vector<std::string>{"MES.v.0", "ZN.v.0"})) << "ES.v.0 has no rows of its own";
    EXPECT_EQ(loaded.value().size(), 8u);
    EXPECT_EQ(view(loaded.value(), "ES.v.0"), view(loaded.value(), "MES.v.0"))
        << "the same dates, closes and ids on both sides of the listing date";
    EXPECT_EQ(view(loaded.value(), "ES.v.0").size(), 3u);
    EXPECT_EQ(view(loaded.value(), "ZN.v.0").size(), 2u);
}

TEST_F(ListingLoaderTest, AListedContractWhosePredecessorIsNotASymbolOfTheRunGetsNoCopy) {
    ListingDates::instance().set({{"MES", "ES", "2026-02-22", 10.0}});
    BacktestDataLoader loader(db_);
    const auto loaded = loader.load_market_data(config_for({"MES.v.0", "ZN.v.0"}));
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    EXPECT_EQ(loaded.value().size(), 5u);
    EXPECT_TRUE(view(loaded.value(), "ES.v.0").empty());
}

// A declared relabel: from its date the new id is read as the old one on the loaded bars of that
// symbol only; closes and dates are untouched, and a predecessor's copy carries the same ids.
TEST_F(ListingLoaderTest, ADeclaredRelabelIsReadOnTheLoadedBars) {
    ListingDates::instance().set_relabels({{"MES", "2026-02-22", "42140878", "42003800"}});
    BacktestDataLoader loader(db_);
    const auto loaded = loader.load_market_data(config_for({"MES.v.0", "ZN.v.0"}));
    ASSERT_TRUE(loaded.is_ok()) << loaded.error()->what();
    EXPECT_EQ(view(loaded.value(), "MES.v.0"),
              (std::vector<std::string>{"2-20 6924 42140878", "2-22 6906 42140878", "2-23 6857 42140878"}));
    EXPECT_EQ(view(loaded.value(), "ZN.v.0"),
              (std::vector<std::string>{"2-20 112 42003800", "2-23 113 42003800"}))
        << "another symbol carrying the same id is not touched";

    ListingDates::instance().set({{"MES", "ES", "2026-02-22", 10.0}});
    const auto paired = loader.load_market_data(config_for({"MES.v.0", "ZN.v.0", "ES.v.0"}));
    ASSERT_TRUE(paired.is_ok()) << paired.error()->what();
    EXPECT_EQ(view(paired.value(), "ES.v.0"),
              (std::vector<std::string>{"2-20 6924 42140878", "2-22 6906 42140878", "2-23 6857 42140878"}));
}
