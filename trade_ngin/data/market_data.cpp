#include "market_data.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/csv/api.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>
#include "database_interface.hpp"

std::vector<MarketData> getMarketData(const std::string& symbol) {
    // Create database connection with environment variables
    std::string conn_str = "postgresql://";
    conn_str += std::getenv("DB_USER") ? std::getenv("DB_USER") : "";
    conn_str += ":";
    conn_str += std::getenv("DB_PASSWORD") ? std::getenv("DB_PASSWORD") : "";
    conn_str += "@";
    conn_str += std::getenv("DB_HOST") ? std::getenv("DB_HOST") : "";
    conn_str += ":";
    conn_str += std::getenv("DB_PORT") ? std::getenv("DB_PORT") : "";
    conn_str += "/";
    conn_str += std::getenv("DB_NAME") ? std::getenv("DB_NAME") : "";

    DatabaseInterface db(conn_str);
    auto table = db.getLatestDataAsArrowTable(symbol);
    
    std::vector<MarketData> data;
    if (!table) return data;
    
    auto timestamp_array = std::static_pointer_cast<arrow::StringArray>(table->GetColumnByName("timestamp")->chunk(0));
    auto symbol_array = std::static_pointer_cast<arrow::StringArray>(table->GetColumnByName("symbol")->chunk(0));
    auto open_array = std::static_pointer_cast<arrow::DoubleArray>(table->GetColumnByName("open")->chunk(0));
    auto high_array = std::static_pointer_cast<arrow::DoubleArray>(table->GetColumnByName("high")->chunk(0));
    auto low_array = std::static_pointer_cast<arrow::DoubleArray>(table->GetColumnByName("low")->chunk(0));
    auto close_array = std::static_pointer_cast<arrow::DoubleArray>(table->GetColumnByName("close")->chunk(0));
    auto volume_array = std::static_pointer_cast<arrow::DoubleArray>(table->GetColumnByName("volume")->chunk(0));
    
    for (int64_t i = 0; i < table->num_rows(); ++i) {
        MarketData bar;
        bar.timestamp = timestamp_array->GetString(i);
        bar.symbol = symbol_array->GetString(i);
        bar.open = open_array->Value(i);
        bar.high = high_array->Value(i);
        bar.low = low_array->Value(i);
        bar.close = close_array->Value(i);
        bar.volume = volume_array->Value(i);
        data.push_back(bar);
    }
    
    return data;
} 