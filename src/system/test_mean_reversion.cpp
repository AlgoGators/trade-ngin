#include "mean_reversion_strategy.hpp"
#include "database_interface.hpp"
#include <arrow/api.h>
#include <iostream>

int main() {
    try {
        // Initialize database interface
        DatabaseInterface db("postgresql://localhost:5432/trade_ngin");

        // Define strategy parameters
        std::unordered_map<std::string, double> ma_params = {
            {"window", 20}  // 20-day moving average
        };

        std::unordered_map<std::string, double> vol_params = {
            {"target_vol", 0.15},    // Target 15% annualized volatility
            {"vol_window", 20}       // 20-day volatility window
        };

        std::unordered_map<std::string, double> zscore_params = {
            {"upper_threshold", 2.0}, // Enter short when z-score > 2
            {"lower_threshold", -2.0},// Enter long when z-score < -2
            {"max_zscore", 3.0}      // Maximum z-score for scaling
        };

        std::unordered_map<std::string, double> weight_params = {
            {"base_size", 0.01}      // Base position size (1% of capital)
        };

        // Initialize strategy
        MeanReversionStrategy strategy;
        strategy.configureSignals(ma_params, vol_params, zscore_params, weight_params);

        // Get list of symbols to trade
        std::vector<std::string> symbols = {
            "6B.c.0", "6C.c.0", "6E.c.0", "6J.c.0", "6M.c.0",
            "6N.c.0", "6S.c.0", "CL.c.0", "GC.c.0", "GF.c.0",
            "HE.c.0", "HG.c.0", "KE.c.0", "LE.c.0", "MES.c.0",
            "MNQ.c.0", "MYM.c.0", "PL.c.0", "RB.c.0", "RTY.c.0",
            "SI.c.0", "UB.c.0", "ZC.c.0", "ZL.c.0", "ZM.c.0",
            "ZN.c.0", "ZR.c.0", "ZS.c.0", "ZW.c.0"
        };

        // Fetch market data
        auto arrow_table = db.getOHLCVArrowTable("2023-01-01", "2023-12-31", symbols);

        // Extract close prices for each symbol
        auto symbol_col = std::static_pointer_cast<arrow::StringArray>(arrow_table->column(1)->chunk(0));
        auto close_col = std::static_pointer_cast<arrow::DoubleArray>(arrow_table->column(5)->chunk(0));
        
        // Group prices by symbol
        std::map<std::string, std::vector<double>> symbol_prices;
        for (int64_t i = 0; i < close_col->length(); ++i) {
            std::string symbol = symbol_col->GetString(i);
            symbol_prices[symbol].push_back(close_col->Value(i));
        }

        // Generate and print signals for each symbol
        std::cout << "Generating mean reversion signals for each symbol:\n";
        for (const auto& symbol : symbols) {
            std::cout << "\nSignals for " << symbol << ":\n";
            
            // Convert prices to MarketData format
            std::vector<MarketData> market_data;
            for (double price : symbol_prices[symbol]) {
                MarketData bar;
                bar.close = price;
                market_data.push_back(bar);
            }
            
            // Generate signals
            auto signals = strategy.generateSignals(market_data);
            
            // Print first 10 non-zero signals
            int count = 0;
            for (size_t i = 0; i < signals.size() && count < 10; ++i) {
                if (signals[i] != 0) {
                    std::cout << "Day " << i << ": Signal = " << signals[i] 
                              << ", Price = " << market_data[i].close << "\n";
                    count++;
                }
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
} 