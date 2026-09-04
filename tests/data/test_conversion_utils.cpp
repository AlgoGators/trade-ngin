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
#include <gtest/gtest.h>

#include <arrow/chunked_array.h>

#include <memory>
#include <vector>

#include "trade_ngin/core/error.hpp"
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

// ===== folded in from tests/data/test_conversion_utils_safe_get.cpp =====
namespace conversion_utils_safe_get_detail {

using namespace trade_ngin;

// Phase 5 §1.17a + §5d -- pins the type-aware Arrow accessors.
//
// LiveDataLoader builds Arrow columns as utf8 via convert_generic_to_arrow,
// and many call sites blind-cast to DoubleArray and silently return 0.0 on
// mismatch. safe_get_double dispatches on the actual column type, accepts
// utf8 as a documented fallback (with WARN on parse failure), and never
// returns a silent default.

namespace {

// Build a single-chunk ChunkedArray<double> from a vector. nulls is parallel.
std::shared_ptr<arrow::ChunkedArray> make_double_column(
    const std::vector<double>& vals, const std::vector<bool>& nulls = {}) {
    arrow::DoubleBuilder b;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (!nulls.empty() && nulls[i]) {
            EXPECT_TRUE(b.AppendNull().ok());
        } else {
            EXPECT_TRUE(b.Append(vals[i]).ok());
        }
    }
    std::shared_ptr<arrow::Array> arr;
    EXPECT_TRUE(b.Finish(&arr).ok());
    return std::make_shared<arrow::ChunkedArray>(arr);
}

std::shared_ptr<arrow::ChunkedArray> make_string_column(
    const std::vector<std::string>& vals, const std::vector<bool>& nulls = {}) {
    arrow::StringBuilder b;
    for (size_t i = 0; i < vals.size(); ++i) {
        if (!nulls.empty() && nulls[i]) {
            EXPECT_TRUE(b.AppendNull().ok());
        } else {
            EXPECT_TRUE(b.Append(vals[i]).ok());
        }
    }
    std::shared_ptr<arrow::Array> arr;
    EXPECT_TRUE(b.Finish(&arr).ok());
    return std::make_shared<arrow::ChunkedArray>(arr);
}

std::shared_ptr<arrow::ChunkedArray> make_int64_column(const std::vector<int64_t>& vals) {
    arrow::Int64Builder b;
    for (auto v : vals) EXPECT_TRUE(b.Append(v).ok());
    std::shared_ptr<arrow::Array> arr;
    EXPECT_TRUE(b.Finish(&arr).ok());
    return std::make_shared<arrow::ChunkedArray>(arr);
}

// Build a multi-chunk ChunkedArray<double> with two chunks of known length so
// we can verify the chunk-walk in resolve_chunk doesn't assume chunk(0).
std::shared_ptr<arrow::ChunkedArray> make_two_chunk_double_column(
    const std::vector<double>& first, const std::vector<double>& second) {
    auto build_one = [](const std::vector<double>& vals) -> std::shared_ptr<arrow::Array> {
        arrow::DoubleBuilder b;
        for (auto v : vals) EXPECT_TRUE(b.Append(v).ok());
        std::shared_ptr<arrow::Array> a;
        EXPECT_TRUE(b.Finish(&a).ok());
        return a;
    };
    return std::make_shared<arrow::ChunkedArray>(
        arrow::ArrayVector{build_one(first), build_one(second)});
}

std::shared_ptr<arrow::ChunkedArray> make_timestamp_column() {
    arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::SECOND), arrow::default_memory_pool());
    EXPECT_TRUE(b.Append(1700000000).ok());
    std::shared_ptr<arrow::Array> arr;
    EXPECT_TRUE(b.Finish(&arr).ok());
    return std::make_shared<arrow::ChunkedArray>(arr);
}

}  // namespace

// DoubleArray is the "easy" path -- unwrap directly, no fallback.
TEST(ConversionUtilsSafeGet, DoubleColumnUnwraps) {
    auto col = make_double_column({1.5, 2.5, 3.5});
    auto r = DataConversionUtils::safe_get_double(col, 1, "test_col");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value(), 2.5);
}

// utf8 column carrying numeric strings is the documented loader storage --
// expected to parse silently via std::stod (no WARN).
TEST(ConversionUtilsSafeGet, StringNumericParses) {
    auto col = make_string_column({"123.45", "67.89"});
    auto r = DataConversionUtils::safe_get_double(col, 0, "px");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value(), 123.45);
}

// utf8 garbage must return an error Result -- NOT a silent 0.0, which was
// the §1.17a regression. (Logger WARN is fired; we don't assert the line
// here because the project's logger doesn't expose a capture API at this
// layer -- the existence of the error Result is the contract.)
TEST(ConversionUtilsSafeGet, StringGarbageReturnsError) {
    auto col = make_string_column({"abc"});
    auto r = DataConversionUtils::safe_get_double(col, 0, "px");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::CONVERSION_ERROR);
}

// Int64 column coerces to double silently -- doubles-on-int columns are a
// common arrow producer quirk and shouldn't WARN.
TEST(ConversionUtilsSafeGet, Int64ColumnCoercesToDouble) {
    auto col = make_int64_column({100, 200, 300});
    auto r = DataConversionUtils::safe_get_double(col, 2, "qty");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value(), 300.0);
}

// Multi-chunk: row index 5 lands in the second chunk (offset 2). If
// resolve_chunk assumed chunk(0), this would either return a wrong value
// or out-of-range error.
TEST(ConversionUtilsSafeGet, MultiChunkResolvesCorrectOffset) {
    auto col = make_two_chunk_double_column({1.0, 2.0, 3.0}, {4.0, 5.0, 6.0});
    auto r = DataConversionUtils::safe_get_double(col, 5, "px");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_DOUBLE_EQ(r.value(), 6.0);
}

// Null cell is an error -- the §1.17a fix forbids the historical silent-0.0
// behavior for nulls just as it does for type mismatches.
TEST(ConversionUtilsSafeGet, NullCellReturnsError) {
    auto col = make_double_column({1.0, 0.0, 3.0}, {false, true, false});
    auto r = DataConversionUtils::safe_get_double(col, 1, "px");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_DATA);
}

// Out-of-range row (past the end of all chunks) is an error.
TEST(ConversionUtilsSafeGet, OutOfRangeRowReturnsError) {
    auto col = make_double_column({1.0, 2.0});
    auto r = DataConversionUtils::safe_get_double(col, 10, "px");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

// Unsupported Arrow type (timestamp) -> ERROR log + error Result.
TEST(ConversionUtilsSafeGet, UnsupportedTypeReturnsError) {
    auto col = make_timestamp_column();
    auto r = DataConversionUtils::safe_get_double(col, 0, "ts");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::CONVERSION_ERROR);
}

// safe_get_int64: doubles in the source are truncated (with a WARN) so
// loaders that pass through fractional ints don't drop the row.
TEST(ConversionUtilsSafeGet, Int64FromDoubleTruncates) {
    auto col = make_double_column({42.9});
    auto r = DataConversionUtils::safe_get_int64(col, 0, "count");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    EXPECT_EQ(r.value(), 42);
}

// safe_get_string: doubles are stringified so callers can store opaque
// label-like values without a separate type branch.
TEST(ConversionUtilsSafeGet, StringFromDoubleStringifies) {
    auto col = make_double_column({1.5});
    auto r = DataConversionUtils::safe_get_string(col, 0, "label");
    ASSERT_TRUE(r.is_ok()) << r.error()->what();
    // std::to_string(1.5) produces "1.500000" with default precision -- the
    // contract is "some string representation", so we accept the natural
    // form rather than pinning a specific format.
    EXPECT_NE(r.value().find("1.5"), std::string::npos);
}

// Null column pointer -> error, not a crash.
TEST(ConversionUtilsSafeGet, NullColumnReturnsError) {
    std::shared_ptr<arrow::ChunkedArray> col;  // nullptr
    auto r = DataConversionUtils::safe_get_double(col, 0, "px");
    ASSERT_TRUE(r.is_error());
    EXPECT_EQ(r.error()->code(), ErrorCode::INVALID_ARGUMENT);
}

}  // namespace conversion_utils_safe_get_detail
