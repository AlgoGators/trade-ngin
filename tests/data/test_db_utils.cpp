#include "test_db_utils.hpp"
#include <arrow/array/builder_primitive.h>
#include <arrow/array/builder_binary.h>
#include <chrono>
#include <arrow/util/logging.h>

namespace trade_ngin {
namespace testing {

// ================= Test Data Implementations =================
std::shared_ptr<arrow::Table> create_test_market_data() {
    arrow::MemoryPool* pool = arrow::default_memory_pool();
    
    auto schema = arrow::schema({
        arrow::field("time", arrow::timestamp(arrow::TimeUnit::SECOND)),
        arrow::field("symbol", arrow::utf8()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::float64())
    });

    arrow::TimestampBuilder timestamp_builder(arrow::timestamp(arrow::TimeUnit::SECOND), pool);
    arrow::StringBuilder symbol_builder(pool);
    arrow::DoubleBuilder open_builder(pool);
    arrow::DoubleBuilder high_builder(pool);
    arrow::DoubleBuilder low_builder(pool);
    arrow::DoubleBuilder close_builder(pool);
    arrow::DoubleBuilder volume_builder(pool);

    const std::vector<int64_t> timestamps = {
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count(),
        std::chrono::duration_cast<std::chrono::seconds>(
            (std::chrono::system_clock::now() + std::chrono::hours(1)).time_since_epoch()).count()
    };

    const std::vector<std::string> symbols = {"AAPL", "GOOG"};
    const std::vector<double> prices = {150.0, 2750.5};
    const std::vector<double> volumes = {1000.0, 500.5};

    for (size_t i = 0; i < 2; ++i) {
        ARROW_CHECK_OK(timestamp_builder.Append(timestamps[i]));
        ARROW_CHECK_OK(symbol_builder.Append(symbols[i]));
        ARROW_CHECK_OK(open_builder.Append(prices[i]));
        ARROW_CHECK_OK(high_builder.Append(prices[i] + 1.0));
        ARROW_CHECK_OK(low_builder.Append(prices[i] - 0.5));
        ARROW_CHECK_OK(close_builder.Append(prices[i] + 0.25));
        ARROW_CHECK_OK(volume_builder.Append(volumes[i]));
    }

    std::shared_ptr<arrow::Array> timestamp_array, symbol_array, 
        open_array, high_array, low_array, close_array, volume_array;

    ARROW_CHECK_OK(timestamp_builder.Finish(&timestamp_array));
    ARROW_CHECK_OK(symbol_builder.Finish(&symbol_array));
    ARROW_CHECK_OK(open_builder.Finish(&open_array));
    ARROW_CHECK_OK(high_builder.Finish(&high_array));
    ARROW_CHECK_OK(low_builder.Finish(&low_array));
    ARROW_CHECK_OK(close_builder.Finish(&close_array));
    ARROW_CHECK_OK(volume_builder.Finish(&volume_array));

    return arrow::Table::Make(schema, {
        timestamp_array, symbol_array, open_array, 
        high_array, low_array, close_array, volume_array
    });
}

std::vector<ExecutionReport> create_test_executions() {
    std::vector<ExecutionReport> executions;
    auto now = std::chrono::system_clock::now();

    executions.push_back({
        "ORD-001", "EXEC-001", "AAPL", Side::BUY,
        100, 150.25, now, 1.50, false
    });

    executions.push_back({
        "ORD-002", "EXEC-002", "MSFT", Side::SELL,
        50, 250.75, now + std::chrono::minutes(5), 2.25, true
    });

    return executions;
}

std::vector<Position> create_test_positions() {
    std::vector<Position> positions;
    auto now = std::chrono::system_clock::now();

    positions.emplace_back("AAPL", 100, 150.0, 500.0, 1000.0, now);
    positions.emplace_back("MSFT", -75, 250.5, -150.0, 500.0, now + std::chrono::hours(1));

    return positions;
}

} // namespace testing
} // namespace trade_ngin