#include "../data/database_client.hpp"
#include "../data/data_interface.hpp"
#include <iostream>
#include <iomanip>
#include <memory>
#include <chrono>
#include <ctime>
#include <arrow/api.h>

int main() {
    auto client = std::make_shared<DatabaseClient>("postgresql://localhost:5432/trade_ngin");
    DataInterface data_interface(client);
    
    // Test database connection and data retrieval
    std::cout << "Testing database connection and data retrieval...\n\n";
    
    // Get current date and date 30 days ago
    auto now = std::chrono::system_clock::now();
    auto end_date = std::chrono::system_clock::to_time_t(now);
    auto start_date = std::chrono::system_clock::to_time_t(now - std::chrono::hours(24 * 30));
    
    // Convert dates to string format YYYY-MM-DD
    char start_date_str[11];
    char end_date_str[11];
    std::strftime(start_date_str, sizeof(start_date_str), "%Y-%m-%d", std::localtime(&start_date));
    std::strftime(end_date_str, sizeof(end_date_str), "%Y-%m-%d", std::localtime(&end_date));
    
    // Fetch OHLCV data for a symbol
    std::string symbol = "ZW.c.0";
    auto data = data_interface.getOHLCV(symbol, start_date_str, end_date_str);
    
    // Print first 10 rows of data with rolling windows
    std::cout << "First 10 rows of OHLCV data for " << symbol << ":\n";
    std::cout << std::setw(20) << "Timestamp" 
              << std::setw(10) << "Open"
              << std::setw(10) << "High" 
              << std::setw(10) << "Low"
              << std::setw(10) << "Close"
              << std::setw(10) << "Volume"
              << std::setw(15) << "MA(10)"
              << std::setw(15) << "MA(20)"
              << std::setw(15) << "Volatility"
              << "\n";
              
    // Calculate some basic indicators
    int short_window = 10;
    int long_window = 20;
    std::vector<double> closes;
    std::vector<double> short_ma;
    std::vector<double> long_ma;
    std::vector<double> volatility;
    
    // Extract close prices from Arrow table
    auto close_array = std::static_pointer_cast<arrow::DoubleArray>(data->GetColumnByName("close")->chunk(0));
    for (int64_t i = 0; i < close_array->length(); ++i) {
        closes.push_back(close_array->Value(i));
    }
    
    // Calculate indicators
    for (size_t i = 0; i < closes.size(); ++i) {
        // Short MA
        if (i >= short_window - 1) {
            double sum = 0;
            for (size_t j = i - short_window + 1; j <= i; ++j) {
                sum += closes[j];
            }
            short_ma.push_back(sum / short_window);
        } else {
            short_ma.push_back(closes[i]);
        }
        
        // Long MA
        if (i >= long_window - 1) {
            double sum = 0;
            for (size_t j = i - long_window + 1; j <= i; ++j) {
                sum += closes[j];
            }
            long_ma.push_back(sum / long_window);
        } else {
            long_ma.push_back(closes[i]);
        }
        
        // Volatility (20-day standard deviation)
        if (i >= long_window - 1) {
            double mean = long_ma.back();
            double sum_sq = 0;
            for (size_t j = i - long_window + 1; j <= i; ++j) {
                sum_sq += (closes[j] - mean) * (closes[j] - mean);
            }
            volatility.push_back(std::sqrt(sum_sq / long_window));
        } else {
            volatility.push_back(0);
        }
    }
    
    // Get timestamp and OHLCV arrays
    auto timestamp_array = std::static_pointer_cast<arrow::StringArray>(data->GetColumnByName("timestamp")->chunk(0));
    auto open_array = std::static_pointer_cast<arrow::DoubleArray>(data->GetColumnByName("open")->chunk(0));
    auto high_array = std::static_pointer_cast<arrow::DoubleArray>(data->GetColumnByName("high")->chunk(0));
    auto low_array = std::static_pointer_cast<arrow::DoubleArray>(data->GetColumnByName("low")->chunk(0));
    auto volume_array = std::static_pointer_cast<arrow::DoubleArray>(data->GetColumnByName("volume")->chunk(0));
    
    // Print first 10 rows with indicators
    for (int64_t i = 0; i < std::min(int64_t(10), data->num_rows()); ++i) {
        std::cout << std::setw(20) << timestamp_array->GetString(i)
                  << std::fixed << std::setprecision(2)
                  << std::setw(10) << open_array->Value(i)
                  << std::setw(10) << high_array->Value(i)
                  << std::setw(10) << low_array->Value(i)
                  << std::setw(10) << close_array->Value(i)
                  << std::setw(10) << volume_array->Value(i)
                  << std::setw(15) << short_ma[i]
                  << std::setw(15) << long_ma[i]
                  << std::setw(15) << volatility[i]
                  << "\n";
    }
    
    std::cout << "\nDatabase test completed successfully.\n";
    return 0;
} 