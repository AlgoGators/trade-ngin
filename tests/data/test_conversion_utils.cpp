// Coverage for conversion_utils.cpp. Targets:
// - arrow_table_to_bars happy path (full row conversion)
// - Missing required column (returns INVALID_DATA)
// - Null table pointer (returns INVALID_ARGUMENT)
// - extract_timestamp / extract_double / extract_string error paths
//   (null array, out-of-range index, null value)
// - Successful single-row extraction

#include <gtest/gtest.h>
#include <arrow/api.h>
#include <arrow/builder.h>
#include <chrono>
#include <memory>

// Pre-load std headers before flipping the macro so libc++ internals stay
// valid; needed because extract_* helpers are private.
#include <string>
#include <vector>
#define private public
#include "trade_ngin/data/conversion_utils.hpp"
#undef private

using namespace trade_ngin;

namespace {

// Build a minimal arrow table with all required Bar columns.
std::shared_ptr<arrow::Table> build_table(int rows, bool null_open = false) {
    arrow::TimestampBuilder time_builder(arrow::timestamp(arrow::TimeUnit::SECOND),
                                          arrow::default_memory_pool());
    arrow::StringBuilder symbol_builder;
    arrow::DoubleBuilder open_builder, high_builder, low_builder, close_builder, volume_builder;

    int64_t base = 1700000000;  // Nov 2023
    for (int i = 0; i < rows; ++i) {
        (void)time_builder.Append(base + i * 60);
        (void)symbol_builder.Append("AAPL");
        if (null_open && i == 0) {
            (void)open_builder.AppendNull();
        } else {
            (void)open_builder.Append(100.0 + i);
        }
        (void)high_builder.Append(101.0 + i);
        (void)low_builder.Append(99.0 + i);
        (void)close_builder.Append(100.5 + i);
        (void)volume_builder.Append(1000.0 + i);
    }

    std::shared_ptr<arrow::Array> time_arr, sym_arr, open_arr, high_arr, low_arr, close_arr,
        volume_arr;
    (void)time_builder.Finish(&time_arr);
    (void)symbol_builder.Finish(&sym_arr);
    (void)open_builder.Finish(&open_arr);
    (void)high_builder.Finish(&high_arr);
    (void)low_builder.Finish(&low_arr);
    (void)close_builder.Finish(&close_arr);
    (void)volume_builder.Finish(&volume_arr);

    auto schema = arrow::schema({
        arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
        arrow::field("symbol", arrow::utf8()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::float64()),
    });
    return arrow::Table::Make(schema,
                              {time_arr, sym_arr, open_arr, high_arr, low_arr, close_arr, volume_arr});
}

}  // namespace

class ConversionUtilsTest : public ::testing::Test {};

// ===== arrow_table_to_bars =====

TEST_F(ConversionUtilsTest, NullTableReturnsInvalidArgumentError) {
    std::shared_ptr<arrow::Table> empty;
    auto r = DataConversionUtils::arrow_table_to_bars(empty);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(ConversionUtilsTest, MissingColumnReturnsInvalidDataError) {
    arrow::DoubleBuilder b;
    (void)b.Append(1.0);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto schema = arrow::schema({arrow::field("only_col", arrow::float64())});
    auto table = arrow::Table::Make(schema, {a});
    auto r = DataConversionUtils::arrow_table_to_bars(table);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(ConversionUtilsTest, ValidTableProducesBars) {
    auto t = build_table(3);
    auto r = DataConversionUtils::arrow_table_to_bars(t);
    ASSERT_TRUE(r.is_ok()) << (r.error() ? r.error()->what() : "");
    EXPECT_EQ(r.value().size(), 3u);
    EXPECT_EQ(r.value()[0].symbol, "AAPL");
    EXPECT_DOUBLE_EQ(r.value()[0].open.to_double(), 100.0);
    EXPECT_DOUBLE_EQ(r.value()[2].close.to_double(), 102.5);
}

TEST_F(ConversionUtilsTest, NullValueInOhlcvReturnsConversionError) {
    auto t = build_table(2, /*null_open=*/true);
    auto r = DataConversionUtils::arrow_table_to_bars(t);
    EXPECT_TRUE(r.is_error());
}

// ===== extract_timestamp =====

TEST_F(ConversionUtilsTest, ExtractTimestampHandlesNullArray) {
    std::shared_ptr<arrow::Array> empty;
    auto r = DataConversionUtils::extract_timestamp(empty, 0);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

TEST_F(ConversionUtilsTest, ExtractTimestampRejectsOutOfRangeIndex) {
    arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::SECOND),
                              arrow::default_memory_pool());
    (void)b.Append(1700000000);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_timestamp(a, 5);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConversionUtilsTest, ExtractTimestampNullValueReturnsInvalidData) {
    arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::SECOND),
                              arrow::default_memory_pool());
    (void)b.AppendNull();
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_timestamp(a, 0);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(ConversionUtilsTest, ExtractTimestampValidValueProducesTimePoint) {
    arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::SECOND),
                              arrow::default_memory_pool());
    (void)b.Append(1700000000);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_timestamp(a, 0);
    ASSERT_TRUE(r.is_ok());
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                     r.value().time_since_epoch())
                     .count();
    EXPECT_EQ(epoch, 1700000000);
}

// ===== extract_double =====

TEST_F(ConversionUtilsTest, ExtractDoubleHandlesNullArray) {
    std::shared_ptr<arrow::Array> empty;
    auto r = DataConversionUtils::extract_double(empty, 0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConversionUtilsTest, ExtractDoubleRejectsOutOfRangeIndex) {
    arrow::DoubleBuilder b;
    (void)b.Append(1.5);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_double(a, 100);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConversionUtilsTest, ExtractDoubleNullValueReturnsInvalidData) {
    arrow::DoubleBuilder b;
    (void)b.AppendNull();
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_double(a, 0);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(ConversionUtilsTest, ExtractDoubleValidValueRoundTrip) {
    arrow::DoubleBuilder b;
    (void)b.Append(3.14159);
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_double(a, 0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_DOUBLE_EQ(r.value(), 3.14159);
}

// ===== extract_string =====

TEST_F(ConversionUtilsTest, ExtractStringHandlesNullArray) {
    std::shared_ptr<arrow::Array> empty;
    auto r = DataConversionUtils::extract_string(empty, 0);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConversionUtilsTest, ExtractStringRejectsOutOfRangeIndex) {
    arrow::StringBuilder b;
    (void)b.Append("hello");
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_string(a, 99);
    EXPECT_TRUE(r.is_error());
}

TEST_F(ConversionUtilsTest, ExtractStringNullValueReturnsInvalidData) {
    arrow::StringBuilder b;
    (void)b.AppendNull();
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_string(a, 0);
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

TEST_F(ConversionUtilsTest, ExtractStringValidValueRoundTrip) {
    arrow::StringBuilder b;
    (void)b.Append("AAPL");
    std::shared_ptr<arrow::Array> a;
    (void)b.Finish(&a);
    auto r = DataConversionUtils::extract_string(a, 0);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), "AAPL");
}
