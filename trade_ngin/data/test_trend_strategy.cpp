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
    double realized_pnl = 0.0;
    int trades = 0;
    int winning_trades = 0;
    
    void updateTrade(double trade_size, double price, bool is_buy) {
        double old_position = position;
        double old_avg_price = avg_price;
        
        // Update position and average price
        if (is_buy) {
            if (position <= 0) {
                // Opening new long or flipping from short
                avg_price = price;
                if (position < 0) {
                    // Realized P&L from closing short
                    double closed_pnl = -position * (price - avg_price);
                    realized_pnl += closed_pnl;
                    if (closed_pnl > 0) winning_trades++;
                    trades++;
                }
            } else {
                // Adding to long
                avg_price = (position * avg_price + trade_size * price) / (position + trade_size);
            }
            position += trade_size;
        } else {
            if (position >= 0) {
                // Opening new short or flipping from long
                avg_price = price;
                if (position > 0) {
                    // Realized P&L from closing long
                    double closed_pnl = position * (price - avg_price);
                    realized_pnl += closed_pnl;
                    if (closed_pnl > 0) winning_trades++;
                    trades++;
                }
            } else {
                // Adding to short
                avg_price = (position * avg_price + trade_size * price) / (position + trade_size);
            }
            position += trade_size;
        }
        
        // Update unrealized P&L
        unrealized_pnl = position * (price - avg_price);
        
        // Update capital weight (using fixed initial capital of 500000.0)
        capital_weight = (position * price) / 500000.0;
    }
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
                
                // Calculate target position
                double capital_per_symbol = initial_capital / all_symbols.size();
                double target_position = signal * capital_per_symbol / price;
                double position_change = target_position - positions[symbol].position;
                
                // Execute mock trade if position change
                if (std::abs(position_change) > 0) {
                    // Mock execution
                    ib.placeOrder(symbol, position_change, price, position_change > 0);
                    
                    // Update position tracking
                    positions[symbol].updateTrade(position_change, price, position_change > 0);
                }
            }
        }

        // Print final portfolio report
        std::cout << "\nFinal Portfolio Report:" << std::endl;
        std::cout << "======================" << std::endl;
        std::cout << "Initial Capital: $" << std::fixed << std::setprecision(2) << initial_capital << std::endl;
        
        // Calculate total P&L and statistics
        double total_realized_pnl = 0.0;
        double total_unrealized_pnl = 0.0;
        int total_trades = 0;
        int total_winning_trades = 0;
        
        for (const auto& [symbol, pos] : positions) {
            total_realized_pnl += pos.realized_pnl;
            total_unrealized_pnl += pos.unrealized_pnl;
            total_trades += pos.trades;
            total_winning_trades += pos.winning_trades;
        }
        
        double current_capital = initial_capital + total_realized_pnl + total_unrealized_pnl;
        double win_rate = total_trades > 0 ? (total_winning_trades * 100.0 / total_trades) : 0.0;
        
        std::cout << "Current Capital: $" << current_capital << std::endl;
        std::cout << "Total Return: " << ((current_capital / initial_capital - 1.0) * 100.0) << "%" << std::endl;
        std::cout << "\nOverall Statistics:" << std::endl;
        std::cout << "Total Trades: " << total_trades << std::endl;
        std::cout << "Win Rate: " << std::fixed << std::setprecision(2) << win_rate << "%" << std::endl;
        std::cout << "Realized P&L: $" << total_realized_pnl << std::endl;
        std::cout << "Unrealized P&L: $" << total_unrealized_pnl << std::endl;
        
        std::cout << "\nPosition Summary:" << std::endl;
        std::cout << "Symbol     Position    Weight     Avg Price    Unrealized P&L    Realized P&L" << std::endl;
        std::cout << "------------------------------------------------------------------------" << std::endl;
        
        for (const auto& [symbol, pos] : positions) {
            std::cout << std::left << std::setw(10) << symbol 
                      << std::right << std::setw(10) << std::fixed << std::setprecision(0) << pos.position 
                      << std::setw(10) << std::fixed << std::setprecision(2) << (pos.capital_weight * 100.0) << "%"
                      << std::setw(12) << std::fixed << std::setprecision(2) << pos.avg_price
                      << std::setw(15) << std::fixed << std::setprecision(2) << pos.unrealized_pnl
                      << std::setw(15) << std::fixed << std::setprecision(2) << pos.realized_pnl << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
} 