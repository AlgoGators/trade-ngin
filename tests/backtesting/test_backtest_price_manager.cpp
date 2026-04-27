#include <gtest/gtest.h>
#include <vector>
#include "../core/test_base.hpp"
#include "trade_ngin/backtest/backtest_price_manager.hpp"

using namespace trade_ngin;
using namespace trade_ngin::backtest;
using namespace trade_ngin::testing;

namespace {

Bar make_bar(const std::string& symbol, double close) {
    Bar bar;
    bar.symbol = symbol;
    bar.timestamp = Timestamp{};
    bar.open = Decimal(close);
    bar.high = Decimal(close);
    bar.low = Decimal(close);
    bar.close = Decimal(close);
    bar.volume = 1000.0;
    return bar;
}

}  // namespace

class BacktestPriceManagerTest : public TestBase {
protected:
    BacktestPriceManager pm_;
};

TEST_F(BacktestPriceManagerTest, EmptyManagerReturnsErrorsAndZeroLengths) {
    EXPECT_TRUE(pm_.get_current_price("ES").is_error());
    EXPECT_TRUE(pm_.get_previous_day_price("ES").is_error());
    EXPECT_TRUE(pm_.get_two_days_ago_price("ES").is_error());
    EXPECT_EQ(pm_.get_price_history("ES"), nullptr);
    EXPECT_EQ(pm_.get_price_history_length("ES"), 0u);
    EXPECT_FALSE(pm_.has_previous_prices());
}

TEST_F(BacktestPriceManagerTest, FirstUpdateSetsCurrentPriceOnly) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    auto cur = pm_.get_current_price("ES");
    ASSERT_TRUE(cur.is_ok());
    EXPECT_DOUBLE_EQ(cur.value(), 4000.0);

    EXPECT_TRUE(pm_.get_previous_day_price("ES").is_error());
    EXPECT_TRUE(pm_.get_two_days_ago_price("ES").is_error());
    EXPECT_FALSE(pm_.has_previous_prices());
}

TEST_F(BacktestPriceManagerTest, SecondUpdateShiftsCurrentToPrevious) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    pm_.update_from_bars({make_bar("ES", 4010.0)});

    auto cur = pm_.get_current_price("ES");
    auto prev = pm_.get_previous_day_price("ES");
    ASSERT_TRUE(cur.is_ok());
    ASSERT_TRUE(prev.is_ok());
    EXPECT_DOUBLE_EQ(cur.value(), 4010.0);
    EXPECT_DOUBLE_EQ(prev.value(), 4000.0);
    EXPECT_TRUE(pm_.get_two_days_ago_price("ES").is_error());
    EXPECT_TRUE(pm_.has_previous_prices());
}

TEST_F(BacktestPriceManagerTest, ThirdUpdateShiftsAllThreeBuckets) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    pm_.update_from_bars({make_bar("ES", 4010.0)});
    pm_.update_from_bars({make_bar("ES", 4020.0)});

    EXPECT_DOUBLE_EQ(pm_.get_current_price("ES").value(), 4020.0);
    EXPECT_DOUBLE_EQ(pm_.get_previous_day_price("ES").value(), 4010.0);
    EXPECT_DOUBLE_EQ(pm_.get_two_days_ago_price("ES").value(), 4000.0);
}

TEST_F(BacktestPriceManagerTest, PriceHistoryAppendsEachUpdate) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    pm_.update_from_bars({make_bar("ES", 4010.0)});
    pm_.update_from_bars({make_bar("ES", 4020.0)});

    const auto* hist = pm_.get_price_history("ES");
    ASSERT_NE(hist, nullptr);
    ASSERT_EQ(hist->size(), 3u);
    EXPECT_DOUBLE_EQ((*hist)[0], 4000.0);
    EXPECT_DOUBLE_EQ((*hist)[1], 4010.0);
    EXPECT_DOUBLE_EQ((*hist)[2], 4020.0);
    EXPECT_EQ(pm_.get_price_history_length("ES"), 3u);
}

TEST_F(BacktestPriceManagerTest, MultiSymbolBatchInOneUpdate) {
    pm_.update_from_bars({make_bar("ES", 4000.0), make_bar("NQ", 15000.0)});
    EXPECT_DOUBLE_EQ(pm_.get_current_price("ES").value(), 4000.0);
    EXPECT_DOUBLE_EQ(pm_.get_current_price("NQ").value(), 15000.0);
    EXPECT_EQ(pm_.get_all_current_prices().size(), 2u);
}

TEST_F(BacktestPriceManagerTest, ShiftPricesWithoutNewBarsAdvancesBuckets) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    pm_.update_from_bars({make_bar("ES", 4010.0)});

    pm_.shift_prices();
    // After shift: previous becomes current(4010), two_days_ago becomes 4000,
    // and current_prices_ is cleared.
    EXPECT_TRUE(pm_.get_current_price("ES").is_error());
    EXPECT_DOUBLE_EQ(pm_.get_previous_day_price("ES").value(), 4010.0);
    EXPECT_DOUBLE_EQ(pm_.get_two_days_ago_price("ES").value(), 4000.0);
    EXPECT_TRUE(pm_.has_previous_prices());
}

TEST_F(BacktestPriceManagerTest, ShiftPricesOnEmptyKeepsHasPreviousFalse) {
    pm_.shift_prices();
    EXPECT_FALSE(pm_.has_previous_prices());
    EXPECT_TRUE(pm_.get_current_price("ES").is_error());
}

TEST_F(BacktestPriceManagerTest, ResetClearsAllStateAndFlag) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    pm_.update_from_bars({make_bar("ES", 4010.0)});
    ASSERT_TRUE(pm_.has_previous_prices());

    pm_.reset();
    EXPECT_FALSE(pm_.has_previous_prices());
    EXPECT_TRUE(pm_.get_current_price("ES").is_error());
    EXPECT_TRUE(pm_.get_previous_day_price("ES").is_error());
    EXPECT_TRUE(pm_.get_two_days_ago_price("ES").is_error());
    EXPECT_EQ(pm_.get_price_history_length("ES"), 0u);
    EXPECT_TRUE(pm_.get_all_current_prices().empty());
    EXPECT_TRUE(pm_.get_all_previous_day_prices().empty());
    EXPECT_TRUE(pm_.get_all_two_days_ago_prices().empty());
}

TEST_F(BacktestPriceManagerTest, GetPriceInterfaceReturnsCurrentRegardlessOfTimestamp) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    auto r = pm_.get_price("ES", Timestamp{});
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(r.value(), 4000.0);

    auto miss = pm_.get_price("UNKNOWN", Timestamp{});
    EXPECT_TRUE(miss.is_error());
}

TEST_F(BacktestPriceManagerTest, GetPricesInterfaceFiltersMissingSymbols) {
    pm_.update_from_bars({make_bar("ES", 4000.0), make_bar("NQ", 15000.0)});
    auto r = pm_.get_prices({"ES", "NQ", "UNKNOWN"}, Timestamp{});
    ASSERT_TRUE(r.is_ok());
    const auto& map = r.value();
    EXPECT_EQ(map.size(), 2u);
    EXPECT_DOUBLE_EQ(map.at("ES"), 4000.0);
    EXPECT_DOUBLE_EQ(map.at("NQ"), 15000.0);
    EXPECT_EQ(map.count("UNKNOWN"), 0u);
}

TEST_F(BacktestPriceManagerTest, GetPricesInterfaceWithEmptySymbolListReturnsEmpty) {
    pm_.update_from_bars({make_bar("ES", 4000.0)});
    auto r = pm_.get_prices({}, Timestamp{});
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value().empty());
}
