// include/data/conversion_utils.hpp
#pragma once

#include <arrow/api.h>
#include <arrow/type_traits.h>
#include <memory>
#include <string>
#include <vector>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

class DataConversionUtils {
public:
    /**
     * @brief Convert Arrow Table to vector of Bars
     * @param table Arrow table containing OHLCV data
     * @return Result containing vector of Bars
     */
    static Result<std::vector<Bar>> arrow_table_to_bars(const std::shared_ptr<arrow::Table>& table);

    // ------------------------------------------------------------
    // Type-safe accessors (Phase 5 §1.17a + §5d)
    //
    // The codebase has a long-standing footgun: LiveDataLoader builds Arrow
    // columns as utf8 (string-of-numbers) via convert_generic_to_arrow, but
    // many call sites do a blind static_pointer_cast<DoubleArray> on
    // chunk(0). On type mismatch this silently returns 0.0 -- corrupt data
    // becomes a zero with no log line.
    //
    // The safe_get_* family below dispatches on the actual Arrow column
    // type, accepts string-storage of numerics as a documented fallback
    // (with WARN telemetry on parse failures), and never returns a silent
    // default on a missing/wrong value -- callers always get a Result error.
    //
    // chunk-aware: resolves (chunk_index, offset) from a logical row index
    // by walking the ChunkedArray, so callers don't have to assume chunk(0).
    // ------------------------------------------------------------

    /**
     * @brief Get a double from a ChunkedArray, with type-aware dispatch.
     *
     * Dispatch rules:
     *   - DOUBLE column            -> unwrap directly.
     *   - INT64 / INT32 column     -> coerce to double (silent promotion).
     *   - STRING column            -> GetString + std::stod; on parse
     *                                 failure, logs WARN naming the bad
     *                                 value and returns an error Result.
     *                                 (Closes §5d -- no more swallowed
     *                                 exceptions.)
     *   - any other Arrow type     -> logs ERROR naming the actual type
     *                                 and returns an error Result.
     *   - null cell                -> error Result, never silently 0.0.
     *
     * @param col          The column to read. Must be non-null.
     * @param row          Logical row index (across all chunks).
     * @param column_name  Human-readable column name for log lines.
     */
    static Result<double> safe_get_double(
        const std::shared_ptr<arrow::ChunkedArray>& col, int64_t row,
        const std::string& column_name);

    /**
     * @brief Get an int64 from a ChunkedArray, with type-aware dispatch.
     *
     * Like safe_get_double but for integers. Doubles in the column are
     * truncated toward zero (with a single WARN about precision loss);
     * strings are parsed via std::stoll.
     */
    static Result<int64_t> safe_get_int64(
        const std::shared_ptr<arrow::ChunkedArray>& col, int64_t row,
        const std::string& column_name);

    /**
     * @brief Get a string from a ChunkedArray.
     *
     * STRING / LARGE_STRING columns are unwrapped directly. Other Arrow
     * types are stringified via their natural representation (e.g. doubles
     * via std::to_string). Null cells are an error.
     */
    static Result<std::string> safe_get_string(
        const std::shared_ptr<arrow::ChunkedArray>& col, int64_t row,
        const std::string& column_name);

    // ------------------------------------------------------------
    // Single-array helpers (kept public so existing arrow_table_to_bars
    // pattern stays usable; promoted from private in Phase 5).
    // ------------------------------------------------------------

    /**
     * @brief Extract timestamp from Arrow array (single chunk).
     * @param array Arrow array containing timestamps
     * @param index Row index
     * @return Result containing timestamp
     */
    static Result<Timestamp> extract_timestamp(const std::shared_ptr<arrow::Array>& array,
                                               int64_t index);

    /**
     * @brief Extract double value from Arrow array (single chunk).
     * @param array Arrow array containing doubles
     * @param index Row index
     * @return Result containing double value
     */
    static Result<double> extract_double(const std::shared_ptr<arrow::Array>& array, int64_t index);

    /**
     * @brief Extract string value from Arrow array (single chunk).
     * @param array Arrow array containing strings
     * @param index Row index
     * @return Result containing string value
     */
    static Result<std::string> extract_string(const std::shared_ptr<arrow::Array>& array,
                                              int64_t index);
};

}  // namespace trade_ngin
