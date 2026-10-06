#include <gtest/gtest.h>
#include <arrow/api.h>
#include <limits>
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/live/live_historical_metrics.hpp"
#include "trade_ngin/storage/live_results_manager.hpp"

using namespace trade_ngin;
namespace {
std::shared_ptr<arrow::Array> strings(std::initializer_list<const char*> cells) {
    arrow::StringBuilder builder;
    for(auto cell:cells) {
        const auto status=cell?builder.Append(cell):builder.AppendNull();
        if(!status.ok())throw std::runtime_error("test Arrow builder failed");
    }
    return builder.Finish().ValueOrDie();
}
}

TEST(EquityModelNumericHistory, ActualNumericDispatchWalksChunkBoundariesAndPreservesNull) {
    auto data=std::make_shared<arrow::ChunkedArray>(std::vector<std::shared_ptr<arrow::Array>>{
        strings({"1.25"}),strings({"-2.5",nullptr,"3.5"})});
    ASSERT_FALSE(DataConversionUtils::safe_get_double(data,1,"cash").is_error());
    EXPECT_DOUBLE_EQ(DataConversionUtils::safe_get_double(data,1,"cash").value(),-2.5);
    EXPECT_TRUE(DataConversionUtils::safe_get_double(data,2,"cash").is_error());
    EXPECT_DOUBLE_EQ(DataConversionUtils::safe_get_double(data,3,"cash").value(),3.5);
}

TEST(EquityModelNumericHistory, InvalidNumericTextCannotProduceFallbackZeroOrPartialValue) {
    for(auto value:{"1.5junk","NaN","inf","-inf","","1e999"}) {
        auto data=std::make_shared<arrow::ChunkedArray>(strings({value}));
        EXPECT_TRUE(DataConversionUtils::safe_get_double(data,0,"cash").is_error())<<value;
    }
}

TEST(EquityModelNumericHistory, InvalidIndexAndMissingColumnAreRefusedBeforeArrowAccess) {
    auto data=std::make_shared<arrow::ChunkedArray>(strings({"1"}));
    EXPECT_TRUE(DataConversionUtils::safe_get_double(data,-1,"cash").is_error());
    EXPECT_TRUE(DataConversionUtils::safe_get_double(data,1,"cash").is_error());
    EXPECT_TRUE(DataConversionUtils::safe_get_double(nullptr,0,"cash").is_error());
}

TEST(EquityModelNumericHistory, TypedNumericFiniteValuesWorkButNonfiniteValuesAreRefused) {
    arrow::DoubleBuilder builder;
    ASSERT_TRUE(builder.Append(12.75).ok());
    ASSERT_TRUE(builder.Append(std::numeric_limits<double>::infinity()).ok());
    auto data=std::make_shared<arrow::ChunkedArray>(builder.Finish().ValueOrDie());
    EXPECT_DOUBLE_EQ(DataConversionUtils::safe_get_double(data,0,"price").value(),12.75);
    EXPECT_TRUE(DataConversionUtils::safe_get_double(data,1,"price").is_error());
    EXPECT_TRUE(DataConversionUtils::safe_get_string(data,0,"symbol").is_error());
}

TEST(EquityModelNumericHistory, StringDispatchWalksChunksWithoutNumericReinterpretation) {
    auto data=std::make_shared<arrow::ChunkedArray>(std::vector<std::shared_ptr<arrow::Array>>{
        strings({"SYN"}),strings({"OTHER",nullptr})});
    EXPECT_EQ(DataConversionUtils::safe_get_string(data,1,"symbol").value(),"OTHER");
    EXPECT_TRUE(DataConversionUtils::safe_get_string(data,2,"symbol").is_error());
}

TEST(EquityModelNumericHistory, UndefinedHistoricalRatiosRemainNullAndAreNotInventedMeasurements) {
    LiveHistoricalMetricsCalculator calculator;
    auto metrics=calculator.calculate({0,0},{0,0},{1000,1000},0,0);
    auto numbers=historical_metrics_double_columns(metrics);
    EXPECT_FALSE(numbers.count("sharpe_ratio"));
    EXPECT_FALSE(numbers.count("sortino_ratio"));
    EXPECT_FALSE(numbers.count("profit_factor"));
    EXPECT_EQ(historical_metrics_null_columns(metrics),
        (std::vector<std::string>{"sharpe_ratio","sortino_ratio","profit_factor"}));
    EXPECT_DOUBLE_EQ(numbers.at("volatility"),0);
}

TEST(EquityModelNumericHistory, PresentHistoricalRatiosAreRetainedExactlyAlongsideMissingRatios) {
    HistoricalMetrics metrics;metrics.sharpe_ratio=1.25;metrics.profit_factor=2.5;
    const auto numbers=historical_metrics_double_columns(metrics);
    EXPECT_DOUBLE_EQ(numbers.at("sharpe_ratio"),1.25);
    EXPECT_DOUBLE_EQ(numbers.at("profit_factor"),2.5);
    EXPECT_EQ(historical_metrics_null_columns(metrics),(std::vector<std::string>{"sortino_ratio"}));
}

TEST(EquityModelNumericHistory, ExplicitEquityNameIsDistinctFromExistingFifthStreamArgument) {
    LiveResultsManager legacy(nullptr,false,"ID","BOOK","qt");
    EXPECT_TRUE(legacy.explicit_strategy_name().empty());
    LiveResultsManager equity(nullptr,false,"ID","BOOK","system","NAME");
    EXPECT_EQ(equity.explicit_strategy_name(),"NAME");
    EXPECT_THROW((LiveResultsManager(nullptr,false,"ID","BOOK","qt","NAME")),std::invalid_argument);
    EXPECT_THROW((LiveResultsManager(nullptr,false,"ID","BOOK","system","")),std::invalid_argument);
}
