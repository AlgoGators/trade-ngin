#include "trade_ngin/data/database_interface.hpp"
#include "trade_ngin/data/market_data.hpp"
#include <iostream>
#include <iomanip>

int main() {
    DatabaseInterface db("postgresql://localhost:5432/trade_ngin");
    
    // Test database connection and data retrieval
    std::cout << "Testing database connection and data retrieval...\n\n";
    
    // Fetch OHLCV data for a symbol
    std::string symbol = "ZW.c.0";
    auto data = db.fetchOHLCVData(symbol);
    
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
    
    // Extract close prices
    for (const auto& row : data) {
        closes.push_back(row["close"].as<double>());
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
    
    // Print first 10 rows with indicators
    for (size_t i = 0; i < std::min(size_t(10), data.size()); ++i) {
        std::cout << std::setw(20) << data[i]["timestamp"].as<std::string>()
                  << std::fixed << std::setprecision(2)
                  << std::setw(10) << data[i]["open"].as<double>()
                  << std::setw(10) << data[i]["high"].as<double>()
                  << std::setw(10) << data[i]["low"].as<double>()
                  << std::setw(10) << data[i]["close"].as<double>()
                  << std::setw(10) << data[i]["volume"].as<double>()
                  << std::setw(15) << short_ma[i]
                  << std::setw(15) << long_ma[i]
                  << std::setw(15) << volatility[i]
                  << "\n";
    }
    
    std::cout << "\nDatabase test completed successfully.\n";
    return 0;
} 