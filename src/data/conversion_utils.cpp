//src/data/conversion_utils.cpp
#include "trade_ngin/data/conversion_utils.hpp"
#include <arrow/type_traits.h>

namespace trade_ngin {

Result<std::vector<Bar>> DataConversionUtils::arrow_table_to_bars(
    const std::shared_ptr<arrow::Table>& table) {

    if (!table) {
        return make_error<std::vector<Bar>>(
            ErrorCode::INVALID_ARGUMENT,
            "Table pointer is null",
            "DataConversionUtils"
        );
    }

    // Verify required columns exist
    std::vector<std::string> required_columns = {
        "time", "symbol", "open", "high", "low", "close", "volume"
    };

    for (const auto& col : required_columns) {
        if (table->GetColumnByName(col) == nullptr) {
            return make_error<std::vector<Bar>>(
                ErrorCode::INVALID_DATA,
                "Missing required column: " + col,
                "DataConversionUtils"
            );
        }
    }

    try {
        // Get column arrays
        auto time_array = table->GetColumnByName("time")->chunk(0);
        auto symbol_array = table->GetColumnByName("symbol")->chunk(0);
        auto open_array = table->GetColumnByName("open")->chunk(0);
        auto high_array = table->GetColumnByName("high")->chunk(0);
        auto low_array = table->GetColumnByName("low")->chunk(0);
        auto close_array = table->GetColumnByName("close")->chunk(0);
        auto volume_array = table->GetColumnByName("volume")->chunk(0);

        // Prepare result vector
        std::vector<Bar> bars;
        bars.reserve(table->num_rows());

        // Convert each row
        for (int64_t i = 0; i < table->num_rows(); ++i) {
            // Extract timestamp
            auto ts_result = extract_timestamp(time_array, i);
            if (ts_result.is_error()) {
                return make_error<std::vector<Bar>>(
                    ts_result.error()->code(),
                    ts_result.error()->what(),
                    "DataConversionUtils"
                );
            }

            // Extract symbol
            auto symbol_result = extract_string(symbol_array, i);
            if (symbol_result.is_error()) {
                return make_error<std::vector<Bar>>(
                    symbol_result.error()->code(),
                    symbol_result.error()->what(),
                    "DataConversionUtils"
                );
            }

            // Extract OHLCV values
            auto open_result = extract_double(open_array, i);
            auto high_result = extract_double(high_array, i);
            auto low_result = extract_double(low_array, i);
            auto close_result = extract_double(close_array, i);
            auto volume_result = extract_double(volume_array, i);

            // Check for errors in OHLCV extraction
            if (open_result.is_error() || high_result.is_error() || 
                low_result.is_error() || close_result.is_error() || 
                volume_result.is_error()) {
                return make_error<std::vector<Bar>>(
                    ErrorCode::CONVERSION_ERROR,
                    "Error extracting OHLCV values at row " + std::to_string(i),
                    "DataConversionUtils"
                );
            }

            // Create and add bar
            Bar bar(
                ts_result.value(),
                open_result.value(),
                high_result.value(),
                low_result.value(),
                close_result.value(),
                volume_result.value(),
                symbol_result.value()
            );

            bars.push_back(bar);
        }

        return Result<std::vector<Bar>>(bars);

    } catch (const std::exception& e) {
        return make_error<std::vector<Bar>>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error converting table to bars: ") + e.what(),
            "DataConversionUtils"
        );
    }
}

Result<Timestamp> DataConversionUtils::extract_timestamp(
    const std::shared_ptr<arrow::Array>& array,
    int64_t index) {

    if (!array || index < 0 || index >= array->length()) {
        return make_error<Timestamp>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid array or index",
            "DataConversionUtils"
        );
    }

    try {
        auto ts_array = std::static_pointer_cast<arrow::TimestampArray>(array);
        if (!ts_array) {
            return make_error<Timestamp>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to cast to timestamp array",
                "DataConversionUtils"
            );
        }

        if (ts_array->IsNull(index)) {
            return make_error<Timestamp>(
                ErrorCode::INVALID_DATA,
                "Null timestamp value at index " + std::to_string(index),
                "DataConversionUtils"
            );
        }

        // Convert timestamp to system_clock::time_point
        auto ts_value = ts_array->Value(index);
        return Result<Timestamp>(
            std::chrono::system_clock::time_point(
                std::chrono::seconds(ts_value)
            )
        );

    } catch (const std::exception& e) {
        return make_error<Timestamp>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error extracting timestamp: ") + e.what(),
            "DataConversionUtils"
        );
    }
}

Result<double> DataConversionUtils::extract_double(
    const std::shared_ptr<arrow::Array>& array,
    int64_t index) {

    if (!array || index < 0 || index >= array->length()) {
        return make_error<double>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid array or index",
            "DataConversionUtils"
        );
    }

    try {
        auto double_array = std::static_pointer_cast<arrow::DoubleArray>(array);
        if (!double_array) {
            return make_error<double>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to cast to double array",
                "DataConversionUtils"
            );
        }

        if (double_array->IsNull(index)) {
            return make_error<double>(
                ErrorCode::INVALID_DATA,
                "Null double value at index " + std::to_string(index),
                "DataConversionUtils"
            );
        }

        return Result<double>(double_array->Value(index));

    } catch (const std::exception& e) {
        return make_error<double>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error extracting double: ") + e.what(),
            "DataConversionUtils"
        );
    }
}

Result<std::string> DataConversionUtils::extract_string(
    const std::shared_ptr<arrow::Array>& array,
    int64_t index) {

    if (!array || index < 0 || index >= array->length()) {
        return make_error<std::string>(
            ErrorCode::INVALID_ARGUMENT,
            "Invalid array or index",
            "DataConversionUtils"
        );
    }

    try {
        auto string_array = std::static_pointer_cast<arrow::StringArray>(array);
        if (!string_array) {
            return make_error<std::string>(
                ErrorCode::CONVERSION_ERROR,
                "Failed to cast to string array",
                "DataConversionUtils"
            );
        }

        if (string_array->IsNull(index)) {
            return make_error<std::string>(
                ErrorCode::INVALID_DATA,
                "Null string value at index " + std::to_string(index),
                "DataConversionUtils"
            );
        }

        return Result<std::string>(string_array->GetString(index));

    } catch (const std::exception& e) {
        return make_error<std::string>(
            ErrorCode::CONVERSION_ERROR,
            std::string("Error extracting string: ") + e.what(),
            "DataConversionUtils"
        );
    }
}

} // namespace trade_ngin