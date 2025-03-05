#pragma once
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/result.h>
#include <arrow/table.h>
#include <memory>
#include <vector>
#include <string>

/*
 * Apache Arrow Tables - Overview
 * -----------------------------
 * Apache Arrow is a columnar memory format for flat and hierarchical data.
 * Key benefits:
 * 1. Zero-copy reads: Data can be accessed without copying/deserializing
 * 2. Columnar format: Efficient for analytical queries and SIMD operations
 * 3. Language interoperability: Same memory format across Python, C++, etc.
 * 4. Memory efficient: Shared memory and memory mapping capabilities
 * 
 * Table Structure:
 * - Schema: Defines the structure (column names and types)
 * - Columns: Contiguous arrays of same-type data
 * - ChunkedArrays: Columns can be split into multiple chunks
 */

class ArrowDataHandler {
public:
    // Struct to hold OHLCV data extracted from Arrow table
    struct OHLCVData {
        std::vector<std::string> timestamps;
        std::vector<double> opens;
        std::vector<double> highs;
        std::vector<double> lows;
        std::vector<double> closes;
        std::vector<int64_t> volumes;
    };

    /*
     * Convert Arrow Table to OHLCV Data
     * --------------------------------
     * Arrow tables store data in a columnar format, which means:
     * - Each column is stored contiguously in memory
     * - Columns can be accessed independently
     * - Data is strongly typed
     * 
     * The table schema should be:
     * - timestamp: string
     * - open: double
     * - high: double
     * - low: double
     * - close: double
     * - volume: int64
     */
    static OHLCVData convertArrowToOHLCV(const std::shared_ptr<arrow::Table>& table) {
        OHLCVData data;
        
        // Get individual columns from the table
        auto timestamp_array = std::static_pointer_cast<arrow::StringArray>(
            table->GetColumnByName("timestamp")->chunk(0));
        auto open_array = std::static_pointer_cast<arrow::DoubleArray>(
            table->GetColumnByName("open")->chunk(0));
        auto high_array = std::static_pointer_cast<arrow::DoubleArray>(
            table->GetColumnByName("high")->chunk(0));
        auto low_array = std::static_pointer_cast<arrow::DoubleArray>(
            table->GetColumnByName("low")->chunk(0));
        auto close_array = std::static_pointer_cast<arrow::DoubleArray>(
            table->GetColumnByName("close")->chunk(0));
        auto volume_array = std::static_pointer_cast<arrow::Int64Array>(
            table->GetColumnByName("volume")->chunk(0));

        // Reserve space for efficiency
        size_t num_rows = table->num_rows();
        data.timestamps.reserve(num_rows);
        data.opens.reserve(num_rows);
        data.highs.reserve(num_rows);
        data.lows.reserve(num_rows);
        data.closes.reserve(num_rows);
        data.volumes.reserve(num_rows);

        // Extract data from Arrow arrays
        for (int64_t i = 0; i < num_rows; ++i) {
            data.timestamps.push_back(timestamp_array->GetString(i));
            data.opens.push_back(open_array->Value(i));
            data.highs.push_back(high_array->Value(i));
            data.lows.push_back(low_array->Value(i));
            data.closes.push_back(close_array->Value(i));
            data.volumes.push_back(volume_array->Value(i));
        }

        return data;
    }

    /*
     * Create Arrow Table from OHLCV Data
     * ---------------------------------
     * This demonstrates how to create an Arrow table from raw data.
     * Useful when you need to:
     * 1. Send data to another system
     * 2. Store data efficiently
     * 3. Interface with Python code
     */
    static std::shared_ptr<arrow::Table> createArrowTable(const OHLCVData& data) {
        // Create Arrow arrays for each column
        arrow::StringBuilder timestamp_builder;
        arrow::DoubleBuilder open_builder;
        arrow::DoubleBuilder high_builder;
        arrow::DoubleBuilder low_builder;
        arrow::DoubleBuilder close_builder;
        arrow::Int64Builder volume_builder;

        // Append data to builders
        ARROW_RETURN_NOT_OK(timestamp_builder.AppendValues(data.timestamps));
        ARROW_RETURN_NOT_OK(open_builder.AppendValues(data.opens));
        ARROW_RETURN_NOT_OK(high_builder.AppendValues(data.highs));
        ARROW_RETURN_NOT_OK(low_builder.AppendValues(data.lows));
        ARROW_RETURN_NOT_OK(close_builder.AppendValues(data.closes));
        ARROW_RETURN_NOT_OK(volume_builder.AppendValues(data.volumes));

        // Finish building arrays
        std::shared_ptr<arrow::Array> timestamp_array;
        std::shared_ptr<arrow::Array> open_array;
        std::shared_ptr<arrow::Array> high_array;
        std::shared_ptr<arrow::Array> low_array;
        std::shared_ptr<arrow::Array> close_array;
        std::shared_ptr<arrow::Array> volume_array;

        ARROW_RETURN_NOT_OK(timestamp_builder.Finish(&timestamp_array));
        ARROW_RETURN_NOT_OK(open_builder.Finish(&open_array));
        ARROW_RETURN_NOT_OK(high_builder.Finish(&high_array));
        ARROW_RETURN_NOT_OK(low_builder.Finish(&low_array));
        ARROW_RETURN_NOT_OK(close_builder.Finish(&close_array));
        ARROW_RETURN_NOT_OK(volume_builder.Finish(&volume_array));

        // Create field vector (schema)
        std::vector<std::shared_ptr<arrow::Field>> schema_vector = {
            arrow::field("timestamp", arrow::utf8()),
            arrow::field("open", arrow::float64()),
            arrow::field("high", arrow::float64()),
            arrow::field("low", arrow::float64()),
            arrow::field("close", arrow::float64()),
            arrow::field("volume", arrow::int64())
        };

        // Create schema and table
        auto schema = std::make_shared<arrow::Schema>(schema_vector);
        return arrow::Table::Make(schema, {
            timestamp_array, open_array, high_array, 
            low_array, close_array, volume_array
        });
    }

    /*
     * Example Usage in Strategy:
     * 
     * void processData(std::shared_ptr<arrow::Table> market_data) {
     *     // 1. Convert Arrow table to OHLCV format
     *     auto data = ArrowDataHandler::convertArrowToOHLCV(market_data);
     * 
     *     // 2. Use the data in your strategy
     *     for (size_t i = 0; i < data.closes.size(); ++i) {
     *         // Calculate indicators
     *         // Generate signals
     *         // etc.
     *     }
     * }
     */
}; 