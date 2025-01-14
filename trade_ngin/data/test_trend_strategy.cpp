#include "test_trend_strategy.hpp"
#include "database_interface.hpp"
#include "mock_ib_interface.hpp"  // Mock IB trading interface
#include <memory>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <map>

struct SymbolPosition {
    double position = 0.0;
    double capital_weight = 0.0;
    double avg_price = 0.0;
    double unrealized_pnl = 0.0;
    std::vector<std::tuple<std::string, double, double, double>> history;  // time, position, weight, price
};

int main() {
    try {
        // Initialize database connection
        DatabaseInterface db("postgresql://postgres:algogators@3.140.200.228:5432/algo_data");
        
        // Initialize mock IB interface
        MockIBInterface ib;
        
        // Get data range
        std::string start_date = db.getEarliestDate();
        std::string end_date = db.getLatestDate();
        std::cout << "Database connection successful!" << std::endl;
        std::cout << "Data range: " << start_date << " to " << end_date << std::endl;

        // Get all available symbols
        std::vector<std::string> all_symbols = db.getAllSymbols();
        std::cout << "\nTrading " << all_symbols.size() << " symbols:" << std::endl;
        for (const auto& symbol : all_symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Initialize portfolio tracking
        double initial_capital = 500000.0;  // $500k initial capital
        double current_capital = initial_capital;
        std::map<std::string, SymbolPosition> positions;
        
        // Initialize strategy
        auto strategy = std::make_unique<TrendStrategy>();
        
        // Configure strategy parameters
        std::unordered_map<std::string, double> ma_params = {
            {"short_window_1", 20}, {"short_window_2", 21}, {"short_window_3", 22},
            {"short_window_4", 23}, {"short_window_5", 24}, {"short_window_6", 25},
            {"long_window_1", 252}, {"long_window_2", 504}, {"long_window_3", 756}
        };
        
        std::unordered_map<std::string, double> vol_params = {
            {"window", 20},
            {"target_vol", 0.12},       // 12% target volatility
            {"high_vol_threshold", 0.15},  // 15% annualized
            {"low_vol_threshold", 0.09}  // 9% annualized
        };
        
        std::unordered_map<std::string, double> regime_params = {
            {"threshold", 0.5}  // If combined signal is "weak" vs. vol, cut in half
        };
        
        std::unordered_map<std::string, double> momentum_params = {
            {"lookback", 20}  // 20-day momentum lookback
        };
        
        std::unordered_map<std::string, double> weight_params = {
            {"short_weight", 0.1167},  // 70% / 6 = ~11.67% each short window
            {"long_weight", 0.10}      // 30% / 3 = 10% each long window
        };
        
        strategy->configureSignals(ma_params, vol_params, regime_params, momentum_params, weight_params);

        // Process each symbol
        for (const auto& symbol : all_symbols) {
            // Get market data for symbol
            auto data_table = db.getOHLCVArrowTable(start_date, end_date, {symbol});
            std::vector<MarketData> market_data;
            
            for (int64_t i = 0; i < data_table->num_rows(); ++i) {
                MarketData bar;
                bar.timestamp = std::static_pointer_cast<arrow::StringArray>(data_table->column(0)->chunk(0))->GetString(i);
                bar.open      = std::static_pointer_cast<arrow::DoubleArray>(data_table->column(1)->chunk(0))->Value(i);
                bar.high      = std::static_pointer_cast<arrow::DoubleArray>(data_table->column(2)->chunk(0))->Value(i);
                bar.low       = std::static_pointer_cast<arrow::DoubleArray>(data_table->column(3)->chunk(0))->Value(i);
                bar.close     = std::static_pointer_cast<arrow::DoubleArray>(data_table->column(4)->chunk(0))->Value(i);
                bar.volume    = std::static_pointer_cast<arrow::DoubleArray>(data_table->column(5)->chunk(0))->Value(i);
                bar.symbol    = symbol;
                market_data.push_back(bar);
            }

            // Generate signals for this symbol
            auto signals = strategy->generateSignals(market_data);

            // Track positions and execute mock trades
            for (size_t i = 1; i < market_data.size(); ++i) {
                double signal = signals[i];
                double price = market_data[i].close;
                double prev_price = market_data[i-1].close;
                
                // Calculate target position
                double capital_per_symbol = current_capital / all_symbols.size();
                double target_position = signal * capital_per_symbol / price;
                double position_change = target_position - positions[symbol].position;
                
                // Execute mock trade if position change
                if (std::abs(position_change) > 0) {
                    // Mock execution
                    ib.placeOrder(symbol, position_change, price);
                    
                    // Update position tracking
                    positions[symbol].position = target_position;
                    positions[symbol].avg_price = price;
                    positions[symbol].capital_weight = (target_position * price) / current_capital;
                    
                    // Record in history
                    positions[symbol].history.push_back(std::make_tuple(
                        market_data[i].timestamp,
                        target_position,
                        positions[symbol].capital_weight,
                        price
                    ));
                }
                
                // Update P&L
                double pnl = positions[symbol].position * (price - prev_price);
                current_capital += pnl;
                positions[symbol].unrealized_pnl += pnl;
            }
        }

        // Print final portfolio report
        std::cout << "\nFinal Portfolio Report:" << std::endl;
        std::cout << "======================" << std::endl;
        std::cout << "Initial Capital: $" << std::fixed << std::setprecision(2) << initial_capital << std::endl;
        std::cout << "Final Capital: $" << current_capital << std::endl;
        std::cout << "Total Return: " << ((current_capital / initial_capital - 1.0) * 100.0) << "%" << std::endl;
        
        std::cout << "\nPosition Summary:" << std::endl;
        std::cout << "Symbol     Position    Weight     Avg Price    Unrealized P&L" << std::endl;
        std::cout << "------------------------------------------------------------" << std::endl;
        
        for (const auto& [symbol, pos] : positions) {
            std::cout << std::left << std::setw(10) << symbol 
                      << std::right << std::setw(10) << std::fixed << std::setprecision(0) << pos.position 
                      << std::setw(10) << std::fixed << std::setprecision(2) << (pos.capital_weight * 100.0) << "%"
                      << std::setw(12) << std::fixed << std::setprecision(2) << pos.avg_price
                      << std::setw(15) << std::fixed << std::setprecision(2) << pos.unrealized_pnl << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
} 