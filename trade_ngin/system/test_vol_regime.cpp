#include "volatility_regime.hpp"
#include "../data/data_interface.hpp"
#include <arrow/api.h>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <algorithm>

void printVolatilityRegimes(const std::string& symbol,
                           const std::vector<double>& prices,
                           const std::vector<double>& returns,
                           const std::vector<double>& fast_vol,
                           const std::vector<double>& slow_vol,
                           const std::vector<double>& multipliers) {
    
    std::cout << "\n=== Volatility Regime Analysis for " << symbol << " ===\n";
    
    // Print header
    std::cout << std::setw(10) << "Day"
              << std::setw(15) << "Price"
              << std::setw(15) << "Return"
              << std::setw(15) << "Fast Vol"
              << std::setw(15) << "Slow Vol"
              << std::setw(15) << "Multiplier"
              << "\n";
              
    // Print data for visualization
    int window = 60;  // Show last 60 days
    size_t start = std::max(0, static_cast<int>(prices.size()) - window);
    
    for (size_t i = start; i < prices.size(); ++i) {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(10) << i
                  << std::setw(15) << prices[i]
                  << std::setw(15) << (i > 0 ? returns[i-1] * 100 : 0)
                  << std::setw(15) << fast_vol[i] * 100
                  << std::setw(15) << slow_vol[i] * 100
                  << std::setw(15) << multipliers[i]
                  << "\n";
    }
    
    // Calculate multiplier statistics
    double avg_multiplier = std::accumulate(multipliers.begin(), multipliers.end(), 0.0) / multipliers.size();
    double min_multiplier = *std::min_element(multipliers.begin(), multipliers.end());
    double max_multiplier = *std::max_element(multipliers.begin(), multipliers.end());
    
    // Print summary statistics
    std::cout << "\nMultiplier Summary:\n";
    std::cout << "Average Multiplier: " << avg_multiplier << "\n";
    std::cout << "Minimum Multiplier: " << min_multiplier << "\n";
    std::cout << "Maximum Multiplier: " << max_multiplier << "\n";
}

int main() {
    try {
        // Initialize database interface with a shared pointer to DatabaseClient
        auto db_client = std::make_shared<DatabaseClient>("postgresql://localhost:5432/trade_ngin");
        DataInterface db(db_client);
        
        // Configure volatility regime detection
        VolatilityRegime::RegimeConfig config;
        // Using default values from constructor
        
        VolatilityRegime vol_regime(config);
        
        // Test with a few key symbols
        std::vector<std::string> test_symbols = {"GC.c.0", "CL.c.0", "ZW.c.0"};
        
        for (const auto& symbol : test_symbols) {
            // Fetch data
            auto arrow_table = db.getOHLCV(symbol, "2023-01-01", "2023-12-31");
            
            // Extract close prices
            auto close_col = std::static_pointer_cast<arrow::DoubleArray>(arrow_table->column(5)->chunk(0));
            std::vector<double> prices;
            for (int64_t i = 0; i < close_col->length(); ++i) {
                prices.push_back(close_col->Value(i));
            }
            
            // Calculate returns
            std::vector<double> returns;
            returns.reserve(prices.size() - 1);
            for (size_t i = 1; i < prices.size(); ++i) {
                returns.push_back((prices[i] / prices[i-1]) - 1.0);
            }
            
            // Calculate volatility and multipliers
            std::vector<double> historical_vol;
            std::vector<double> multipliers;
            
            // Use a rolling window to calculate historical volatility
            const int vol_window = 20;  // 20-day volatility
            for (size_t i = vol_window; i < returns.size(); ++i) {
                // Calculate volatility for this window
                double sum_squared = 0.0;
                for (size_t j = i - vol_window; j < i; ++j) {
                    sum_squared += returns[j] * returns[j];
                }
                double vol = std::sqrt(sum_squared / vol_window) * std::sqrt(252.0);  // Annualize
                historical_vol.push_back(vol);
                
                // Calculate multiplier based on current vol and history
                double multiplier = vol_regime.calculateVolMultiplier(vol, historical_vol);
                multipliers.push_back(multiplier);
            }
            
            // Pad the beginning with the first calculated values
            if (!historical_vol.empty()) {
                historical_vol.insert(historical_vol.begin(), vol_window, historical_vol.front());
                multipliers.insert(multipliers.begin(), vol_window, multipliers.front());
            }
            
            // Print analysis
            printVolatilityRegimes(symbol, prices, returns, historical_vol, historical_vol, multipliers);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
} 