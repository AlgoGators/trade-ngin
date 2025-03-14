#include "test_trend_strategy.hpp"
#include "../data/ohlcv_data_handler.hpp"
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

std::vector<double> TrendStrategy::generateSignals(const std::vector<MarketData>& data) {
    std::vector<double> signals(data.size(), 0.0);
    if (data.size() < 30) return signals;

    // Parameters for signal generation
    const int fast_period = 5;
    const int slow_period = 15;
    const int vol_window = 10;
    const int momentum_days = 3;

    // Calculate EMAs and volatility
    std::vector<double> fast_ema(data.size(), 0.0);
    std::vector<double> slow_ema(data.size(), 0.0);
    std::vector<double> volatility(data.size(), 0.0);
    std::vector<double> returns(data.size(), 0.0);

    // Calculate returns
    for (size_t i = 1; i < data.size(); i++) {
        returns[i] = (data[i].close - data[i-1].close) / data[i-1].close;
    }

    // Initialize EMAs
    fast_ema[0] = data[0].close;
    slow_ema[0] = data[0].close;

    // Calculate EMAs and volatility
    for (size_t i = 1; i < data.size(); i++) {
        // Update EMAs
        double fast_alpha = 2.0 / (fast_period + 1);
        double slow_alpha = 2.0 / (slow_period + 1);
        fast_ema[i] = data[i].close * fast_alpha + fast_ema[i-1] * (1 - fast_alpha);
        slow_ema[i] = data[i].close * slow_alpha + slow_ema[i-1] * (1 - slow_alpha);

        // Calculate volatility (standard deviation of returns)
        if (i >= vol_window) {
            double sum = 0.0;
            double sum_sq = 0.0;
            for (int j = 0; j < vol_window; j++) {
                sum += returns[i-j];
                sum_sq += returns[i-j] * returns[i-j];
            }
            double mean = sum / vol_window;
            volatility[i] = std::sqrt(sum_sq/vol_window - mean*mean);
        }
    }

    // Generate signals
    for (size_t i = momentum_days; i < data.size(); i++) {
        // Calculate trend signal
        double trend = (fast_ema[i] - slow_ema[i]) / slow_ema[i];
        
        // Calculate momentum (average of recent returns)
        double momentum = 0.0;
        for (int j = 0; j < momentum_days; j++) {
            momentum += returns[i-j];
        }
        momentum /= momentum_days;

        // Combine signals with dynamic scaling based on volatility
        double vol_scale = 1.0;
        if (volatility[i] > 0) {
            vol_scale = 0.02 / volatility[i]; // Target 2% volatility
        }

        // Generate raw signal
        double raw_signal = (trend * 30.0 + momentum * 70.0) * vol_scale;
        
        // Apply position limits
        signals[i] = std::max(-20.0, std::min(20.0, raw_signal));
    }

    return signals;
}

void TrendStrategy::runBacktest(const std::string& connection_string) {
    try {
        // Initialize database connection
        OHLCVDataHandler db(connection_string);
        
        // Initialize mock IB interface
        MockIBInterface ib;
        
        // Get data range
        std::string start_date = db.getEarliestDate();
        std::string end_date = db.getLatestDate();
        std::cout << "Database connection successful!" << std::endl;
        std::cout << "Data range: " << start_date << " to " << end_date << std::endl;

        // Get all symbols from the database
        auto symbols_table = db.getSymbolsAsArrowTable();
        auto chunked_array = symbols_table->column(0);
        std::vector<std::string> all_symbols;
        for (int c = 0; c < chunked_array->num_chunks(); ++c) {
            auto chunk = std::static_pointer_cast<arrow::StringArray>(chunked_array->chunk(c));
            for (int64_t i = 0; i < chunk->length(); ++i) {
                all_symbols.push_back(chunk->GetString(i));
            }
        }
        std::cout << "\nTrading " << all_symbols.size() << " symbols:" << std::endl;
        for (const auto& symbol : all_symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Initialize portfolio tracking
        double initial_capital = 500000.0;  // $500k initial capital
        std::map<std::string, SymbolPosition> positions;
        
        // Get data for each symbol and generate signals
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
            auto signals = generateSignals(market_data);

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
        std::cout << "Win Rate: " << win_rate << "%" << std::endl;
        
        // Print individual symbol statistics
        std::cout << "\nSymbol Statistics:" << std::endl;
        std::cout << "Symbol\tPosition\tCapital Weight\tRealized P&L\tUnrealized P&L\tTrades\tWin Rate" << std::endl;
        for (const auto& [symbol, pos] : positions) {
            double symbol_win_rate = pos.trades > 0 ? (pos.winning_trades * 100.0 / pos.trades) : 0.0;
            std::cout << symbol << "\t"
                     << pos.position << "\t"
                     << pos.capital_weight << "\t"
                     << pos.realized_pnl << "\t"
                     << pos.unrealized_pnl << "\t"
                     << pos.trades << "\t"
                     << symbol_win_rate << "%" << std::endl;
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error in runBacktest: " << e.what() << std::endl;
        throw;
    }
} 