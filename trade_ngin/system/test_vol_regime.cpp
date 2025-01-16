#include "../system/volatility_regime.hpp"
#include "database_interface.hpp"
#include <arrow/api.h>
#include <iostream>
#include <iomanip>

void printVolatilityRegimes(const std::string& symbol,
                           const std::vector<double>& prices,
                           const std::vector<double>& returns,
                           const std::vector<double>& fast_vol,
                           const std::vector<double>& slow_vol,
                           const std::vector<VolatilityRegime::Regime>& regimes) {
    
    std::cout << "\n=== Volatility Regime Analysis for " << symbol << " ===\n";
    
    // Print header
    std::cout << std::setw(10) << "Day"
              << std::setw(15) << "Price"
              << std::setw(15) << "Return"
              << std::setw(15) << "Fast Vol"
              << std::setw(15) << "Slow Vol"
              << std::setw(15) << "Regime"
              << "\n";
              
    // Print data for visualization
    int window = 60;  // Show last 60 days
    size_t start = std::max(0, static_cast<int>(prices.size()) - window);
    
    for (size_t i = start; i < prices.size(); ++i) {
        std::string regime_str;
        if (i >= window) {
            switch (regimes[i]) {
                case VolatilityRegime::Regime::HIGH_VOLATILITY:
                    regime_str = "HIGH";
                    break;
                case VolatilityRegime::Regime::LOW_VOLATILITY:
                    regime_str = "LOW";
                    break;
                default:
                    regime_str = "NORMAL";
            }
        } else {
            regime_str = "N/A";
        }
        
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(10) << i
                  << std::setw(15) << prices[i]
                  << std::setw(15) << (i > 0 ? returns[i-1] * 100 : 0)
                  << std::setw(15) << fast_vol[i] * 100
                  << std::setw(15) << slow_vol[i] * 100
                  << std::setw(15) << regime_str
                  << "\n";
    }
    
    // Calculate regime statistics
    int high_vol_count = 0;
    int low_vol_count = 0;
    int normal_vol_count = 0;
    
    for (const auto& regime : regimes) {
        switch (regime) {
            case VolatilityRegime::Regime::HIGH_VOLATILITY:
                high_vol_count++;
                break;
            case VolatilityRegime::Regime::LOW_VOLATILITY:
                low_vol_count++;
                break;
            case VolatilityRegime::Regime::NORMAL_VOLATILITY:
                normal_vol_count++;
                break;
        }
    }
    
    // Print summary statistics
    std::cout << "\nRegime Summary:\n";
    std::cout << "High Volatility Days: " << high_vol_count << " ("
              << (high_vol_count * 100.0 / regimes.size()) << "%)\n";
    std::cout << "Low Volatility Days: " << low_vol_count << " ("
              << (low_vol_count * 100.0 / regimes.size()) << "%)\n";
    std::cout << "Normal Volatility Days: " << normal_vol_count << " ("
              << (normal_vol_count * 100.0 / regimes.size()) << "%)\n";
}

int main() {
    try {
        // Initialize database interface
        DatabaseInterface db("postgresql://localhost:5432/trade_ngin");
        
        // Configure volatility regime detection
        VolatilityRegime::RegimeConfig config;
        config.fast_window = 20;     // 20-day fast volatility
        config.slow_window = 60;     // 60-day slow volatility
        config.high_threshold = 1.5;  // 50% above baseline for high vol
        config.low_threshold = 0.5;   // 50% below baseline for low vol
        config.use_relative = true;   // Use relative vol comparison
        
        VolatilityRegime vol_regime(config);
        
        // Test with a few key symbols
        std::vector<std::string> test_symbols = {"GC.c.0", "CL.c.0", "ZW.c.0"};
        
        for (const auto& symbol : test_symbols) {
            // Fetch data
            auto arrow_table = db.getOHLCVArrowTable("2023-01-01", "2023-12-31", {symbol});
            
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
            
            // Calculate rolling volatilities
            auto fast_vol = vol_regime.calculateRollingVol(returns, config.fast_window);
            auto slow_vol = vol_regime.calculateRollingVol(returns, config.slow_window);
            
            // Detect regimes
            std::vector<VolatilityRegime::Regime> regimes;
            for (size_t i = 0; i < returns.size(); ++i) {
                auto regime = vol_regime.detectRegime(
                    std::vector<double>(returns.begin(), returns.begin() + i + 1)
                );
                regimes.push_back(regime);
            }
            
            // Print analysis
            printVolatilityRegimes(symbol, prices, returns, fast_vol, slow_vol, regimes);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    
    return EXIT_SUCCESS;
} 