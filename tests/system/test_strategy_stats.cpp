#include "../system/trend_strategy.hpp"
#include "database_interface.hpp"
#include <arrow/api.h>
#include <iostream>
#include <iomanip>

void printStrategyStats(const std::string& symbol, 
                       const std::vector<double>& prices,
                       const std::vector<double>& signals,
                       const std::vector<double>& ma_6day) {
    
    std::cout << "\n=== Strategy Statistics for " << symbol << " ===\n";
    
    // Print header
    std::cout << std::setw(10) << "Day" 
              << std::setw(15) << "Price" 
              << std::setw(15) << "6-day MA"
              << std::setw(15) << "Signal"
              << std::setw(15) << "Position"
              << "\n";
              
    // Print first 20 days of data
    for (size_t i = 6; i < std::min(size_t(26), prices.size()); ++i) {
        std::cout << std::fixed << std::setprecision(2)
                  << std::setw(10) << i 
                  << std::setw(15) << prices[i]
                  << std::setw(15) << ma_6day[i]
                  << std::setw(15) << signals[i]
                  << std::setw(15) << (prices[i] > ma_6day[i] ? "LONG" : "SHORT")
                  << "\n";
    }
    
    // Calculate basic statistics
    int total_signals = 0;
    int long_signals = 0;
    int short_signals = 0;
    double avg_signal_strength = 0.0;
    
    for (size_t i = 6; i < signals.size(); ++i) {
        if (signals[i] != 0) {
            total_signals++;
            if (signals[i] > 0) long_signals++;
            else short_signals++;
            avg_signal_strength += std::abs(signals[i]);
        }
    }
    
    if (total_signals > 0) {
        avg_signal_strength /= total_signals;
    }
    
    // Print summary statistics
    std::cout << "\nSummary Statistics:\n";
    std::cout << "Total Trading Days: " << prices.size() << "\n";
    std::cout << "Total Signals Generated: " << total_signals << "\n";
    std::cout << "Long Signals: " << long_signals << "\n";
    std::cout << "Short Signals: " << short_signals << "\n";
    std::cout << std::fixed << std::setprecision(4)
              << "Average Signal Strength: " << avg_signal_strength << "\n";
    
    // Calculate price movement statistics
    double total_up_moves = 0;
    double total_down_moves = 0;
    int up_days = 0;
    int down_days = 0;
    
    for (size_t i = 1; i < prices.size(); ++i) {
        double move = prices[i] - prices[i-1];
        if (move > 0) {
            total_up_moves += move;
            up_days++;
        } else if (move < 0) {
            total_down_moves += std::abs(move);
            down_days++;
        }
    }
    
    std::cout << "\nPrice Movement Statistics:\n";
    std::cout << "Up Days: " << up_days << "\n";
    std::cout << "Down Days: " << down_days << "\n";
    std::cout << std::fixed << std::setprecision(2)
              << "Average Up Move: " << (up_days > 0 ? total_up_moves/up_days : 0) << "\n"
              << "Average Down Move: " << (down_days > 0 ? total_down_moves/down_days : 0) << "\n";
}

int main() {
    try {
        // Initialize database interface
        DatabaseInterface db("postgresql://localhost:5432/trade_ngin");
        
        // Define strategy parameters for 6-day MA
        std::unordered_map<std::string, double> ma_params = {
            {"short_window_1", 6},    // 6-day MA
            {"short_window_2", 6},
            {"short_window_3", 6},
            {"short_window_4", 6},
            {"short_window_5", 6},
            {"short_window_6", 6},
            {"long_window_1", 6},
            {"long_window_2", 6},
            {"long_window_3", 6}
        };

        std::unordered_map<std::string, double> vol_params = {
            {"window", 6},            // 6-day volatility window
            {"target_vol", 0.15},     // Target 15% annualized volatility
            {"high_vol_threshold", 1.5},
            {"low_vol_threshold", 0.5}
        };

        std::unordered_map<std::string, double> regime_params = {
            {"lookback", 6},          // 6-day regime window
            {"threshold", 0.02}
        };

        std::unordered_map<std::string, double> momentum_params = {
            {"lookback", 6},          // 6-day momentum window
            {"threshold", 0.02}
        };

        std::unordered_map<std::string, double> weight_params = {
            {"short_weight", 0.6},
            {"long_weight", 0.4},
            {"base_size", 0.01}
        };

        // Initialize strategy
        TrendStrategy strategy;
        strategy.configureSignals(ma_params, vol_params, regime_params, momentum_params, weight_params);

        // Test with a few key symbols
        std::vector<std::string> test_symbols = {"GC.c.0", "CL.c.0", "ZW.c.0"};
        
        for (const auto& symbol : test_symbols) {
            // Fetch data for one symbol
            auto arrow_table = db.getOHLCVArrowTable("2023-01-01", "2023-12-31", {symbol});
            
            // Extract close prices
            auto close_col = std::static_pointer_cast<arrow::DoubleArray>(arrow_table->column(5)->chunk(0));
            std::vector<double> prices;
            for (int64_t i = 0; i < close_col->length(); ++i) {
                prices.push_back(close_col->Value(i));
            }
            
            // Convert to MarketData format
            std::vector<MarketData> market_data;
            for (double price : prices) {
                MarketData bar;
                bar.close = price;
                market_data.push_back(bar);
            }
            
            // Generate signals
            auto signals = strategy.generateSignals(market_data);
            
            // Calculate 6-day MA for comparison
            std::vector<double> ma_6day(prices.size(), 0.0);
            for (size_t i = 5; i < prices.size(); ++i) {
                double sum = 0;
                for (int j = 0; j < 6; ++j) {
                    sum += prices[i - j];
                }
                ma_6day[i] = sum / 6;
            }
            
            // Print statistics
            printStrategyStats(symbol, prices, signals, ma_6day);
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
} 