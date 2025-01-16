#include "test_trend_strategy.hpp"
#include "database_interface.hpp"
#include "mock_ib_interface.hpp"
#include <memory>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <map>
#include <unordered_map>
#include <chrono>
#include <thread>

struct SymbolPosition {
    double position = 0.0;
    double signal = 0.0;
    double avg_price = 0.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    double capital_weight = 0.0;
    int total_trades = 0;
    int trades = 0;
    int winning_trades = 0;
    double max_profit_trade = 0.0;
    double max_loss_trade = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double avg_hold_time_wins = 0.0;
    double avg_hold_time_losses = 0.0;
    std::string last_trade_time;
    std::vector<std::tuple<std::string, double, double>> history;
    bool in_position = false;
};

// Global map to store positions for each symbol
std::unordered_map<std::string, SymbolPosition> positions;

// Add capital as a global constant
const double initial_capital = 500000.0;

// Global parameters
std::unordered_map<std::string, double> ma_params = {
    {"short_window_1", 10}, {"short_window_2", 20}, {"short_window_3", 30},
    {"short_window_4", 40}, {"short_window_5", 50}, {"short_window_6", 60},
    {"long_window_1", 100}, {"long_window_2", 200}, {"long_window_3", 300}
};

std::unordered_map<std::string, double> vol_params = {
    {"window", 20},
    {"target_vol", 0.20},       // Increased target volatility to 20%
    {"high_vol_threshold", 1.5}, // More tolerant high vol threshold
    {"low_vol_threshold", 0.5}   // More tolerant low vol threshold
};

std::unordered_map<std::string, double> regime_params = {
    {"trend_threshold", 0.05},   // Minimum trend strength required
    {"vol_target", 0.20},       // Target volatility
    {"max_leverage", 2.0}       // Maximum leverage
};

std::unordered_map<std::string, double> momentum_params = {
    {"lookback", 60},           // Momentum lookback period
    {"threshold", 0.02}         // Momentum signal threshold
};

std::unordered_map<std::string, double> weight_params = {
    {"short_weight", 0.15},     // Weight for each short-term signal
    {"long_weight", 0.10},      // Weight for each long-term signal
    {"base_size", 0.005}        // Increased base position size to 0.5% of capital
};

// Add contract multipliers
std::unordered_map<std::string, double> contract_multipliers = {
    {"6B", 62500.0}, {"6E", 125000.0}, {"6J", 12500000.0}, {"6C", 100000.0},
    {"6M", 500000.0}, // Added Mexican Peso
    {"CL", 1000.0}, {"GC", 100.0}, {"SI", 5000.0}, {"ZW", 50.0},
    {"ZC", 50.0}, {"ZS", 50.0}, {"HG", 25000.0}, {"PL", 50.0}
};

void printPortfolioReport(const std::map<std::string, SymbolPosition>& positions, double initial_capital) {
    std::cout << "\nPortfolio Report - ";
    time_t now = std::time(nullptr);
    std::cout << std::put_time(std::localtime(&now), "%Y-%m-%d %H:%M:%S") << std::endl;
    std::cout << "======================" << std::endl;
    std::cout << "Initial Capital: $" << std::fixed << std::setprecision(2) << initial_capital << std::endl;
    std::cout << "Current Capital: $" << initial_capital << std::endl;
    std::cout << "Total Return: " << ((initial_capital / initial_capital - 1.0) * 100.0) << "%" << std::endl;
    
    // Calculate portfolio stats
    int total_trades = 0;
    int total_winning_trades = 0;
    double total_realized_pnl = 0.0;
    double total_unrealized_pnl = 0.0;
    
    for (const auto& [symbol, pos] : positions) {
        total_trades += pos.trades;
        total_winning_trades += pos.winning_trades;
        total_realized_pnl += pos.realized_pnl;
        total_unrealized_pnl += pos.unrealized_pnl;
    }
    
    std::cout << "\nOverall Statistics:" << std::endl;
    std::cout << "Total Trades: " << total_trades << std::endl;
    std::cout << "Win Rate: " << (total_trades > 0 ? (total_winning_trades * 100.0 / total_trades) : 0.0) << "%" << std::endl;
    std::cout << "Realized P&L: $" << total_realized_pnl << std::endl;
    std::cout << "Unrealized P&L: $" << total_unrealized_pnl << std::endl;
    
    std::cout << "\nPosition Summary:" << std::endl;
    std::cout << std::left << std::setw(10) << "Symbol" 
              << std::right << std::setw(10) << "Position"
              << std::setw(10) << "Weight"
              << std::setw(12) << "Avg Price"
              << std::setw(15) << "Unreal P&L"
              << std::setw(15) << "Real P&L"
              << std::setw(10) << "Trades"
              << std::setw(10) << "Win %" << std::endl;
    std::cout << "--------------------------------------------------------------------------------" << std::endl;
    
    for (const auto& [symbol, pos] : positions) {
        std::cout << std::left << std::setw(10) << symbol 
                  << std::right << std::setw(10) << std::fixed << std::setprecision(0) << pos.position 
                  << std::setw(9) << std::fixed << std::setprecision(1) << (pos.capital_weight * 100.0) << "%"
                  << std::setw(12) << std::fixed << std::setprecision(2) << pos.avg_price
                  << std::setw(15) << std::fixed << std::setprecision(2) << pos.unrealized_pnl
                  << std::setw(15) << pos.realized_pnl
                  << std::setw(10) << pos.trades
                  << std::setw(9) << (pos.trades > 0 ? (pos.winning_trades * 100.0 / pos.trades) : 0.0) << "%" 
                  << std::endl;
    }
    
    std::cout << "\nDetailed Trade Analysis:" << std::endl;
    std::cout << std::left << std::setw(10) << "Symbol" 
              << std::right << std::setw(12) << "Avg Win"
              << std::setw(12) << "Avg Loss"
              << std::setw(12) << "Max Win"
              << std::setw(12) << "Max Loss"
              << std::setw(15) << "Hold Time W"
              << std::setw(15) << "Hold Time L" << std::endl;
    std::cout << "--------------------------------------------------------------------------------" << std::endl;
    
    for (const auto& [symbol, pos] : positions) {
        if (pos.trades > 0) {
            std::cout << std::left << std::setw(10) << symbol 
                      << std::right << std::setw(12) << std::fixed << std::setprecision(2) << pos.avg_win
                      << std::setw(12) << pos.avg_loss
                      << std::setw(12) << pos.max_profit_trade
                      << std::setw(12) << pos.max_loss_trade
                      << std::setw(15) << std::fixed << std::setprecision(1) << pos.avg_hold_time_wins
                      << std::setw(15) << pos.avg_hold_time_losses << std::endl;
        }
    }
}

// Calculate dynamic position size based on win rate and volatility
double getPositionSize(const SymbolPosition& pos, double vol_scalar, double price) {
    double base_size = initial_capital * 0.01;  // Base position size 1% of capital
    double win_rate_scalar = pos.total_trades > 10 ? 
        (static_cast<double>(pos.winning_trades) / pos.total_trades) : 0.5;
    
    // Scale up position size for higher win rates
    double position_scalar = 1.0;
    if (win_rate_scalar > 0.5) {
        position_scalar = 1.0 + (win_rate_scalar - 0.5) * 6.0;  // Up to 4x for high win rates
    } else {
        position_scalar = 0.5 + win_rate_scalar;  // Reduce size for low win rates
    }
    
    // Apply volatility scaling with less reduction
    double vol_adjusted_size = base_size * position_scalar * std::max(0.7, vol_scalar) / price;
    
    // Apply maximum position size limit - increased to 5% per position
    return std::min(vol_adjusted_size, initial_capital * 0.05 / price);
}

void updatePosition(const std::string& symbol, double new_signal, double price, const std::string& timestamp) {
    auto& pos = positions[symbol];
    pos.signal = new_signal;
    
    // Get contract multiplier
    double multiplier = 1.0;
    for (const auto& [prefix, mult] : contract_multipliers) {
        if (symbol.substr(0, prefix.length()) == prefix) {
            multiplier = mult;
            break;
        }
    }
    
    // Check for position exit
    if (pos.in_position) {
        double unrealized_profit = (price - pos.avg_price) * pos.position * multiplier;
        double position_value = std::abs(pos.position) * price * multiplier;
        
        if ((unrealized_profit >= position_value * 0.10) ||  // Take profit at 10%
            (unrealized_profit <= -position_value * 0.05) ||  // Stop loss at 5%
            (pos.signal * pos.position < 0 && std::abs(pos.signal) > 0.8)) {  // Strong reversal signal
            
            // Update trade statistics
            pos.realized_pnl += unrealized_profit;
            pos.trades++;
            if (unrealized_profit > 0) {
                pos.winning_trades++;
                pos.avg_win = (pos.avg_win * (pos.winning_trades - 1) + unrealized_profit) / pos.winning_trades;
                pos.max_profit_trade = std::max(pos.max_profit_trade, unrealized_profit);
            } else {
                pos.avg_loss = (pos.avg_loss * (pos.trades - pos.winning_trades - 1) + unrealized_profit) / (pos.trades - pos.winning_trades);
                pos.max_loss_trade = std::min(pos.max_loss_trade, unrealized_profit);
            }
            
            pos.history.push_back(std::make_tuple(timestamp, 0.0, price));
            pos.position = 0.0;
            pos.unrealized_pnl = 0.0;
            pos.in_position = false;
            pos.capital_weight = 0.0;
            return;
        }
    }
    
    // Calculate new position size based on signal and contract value
    double notional_value = price * multiplier;
    double max_position = initial_capital * 0.02 / notional_value;  // Max 2% of capital per position
    
    // Apply position size adjustments for different instruments
    if (symbol.substr(0, 2) == "6J" || symbol.substr(0, 2) == "6E" || 
        symbol.substr(0, 2) == "6B" || symbol.substr(0, 2) == "6C" ||
        symbol.substr(0, 2) == "6M") {
        max_position *= 0.3;  // Reduce currency futures by 70%
    } else if (symbol.substr(0, 2) == "CL") {
        max_position *= 1.5;  // Increase crude oil by 50%
    } else if (symbol.substr(0, 2) == "GC" || symbol.substr(0, 2) == "SI") {
        max_position *= 1.2;  // Increase precious metals by 20%
    }
    
    double target_position = max_position * pos.signal;
    double position_change = target_position - pos.position;
    
    // Only trade if position change is significant (0.1% of position value)
    double min_change = 0.001 * initial_capital / notional_value;
    
    if (std::abs(position_change) > min_change) {
        // If entering a new position
        if (!pos.in_position && std::abs(target_position) > min_change) {
            pos.trades++;
            pos.in_position = true;
            pos.avg_price = price;
            pos.position = target_position;
            pos.last_trade_time = timestamp;
            pos.history.push_back(std::make_tuple(timestamp, target_position, price));
        }
        // If modifying existing position
        else if (pos.in_position) {
            // Update average price if increasing position
            if (std::abs(target_position) > std::abs(pos.position)) {
                pos.avg_price = (pos.avg_price * std::abs(pos.position) + price * std::abs(position_change)) / 
                               std::abs(target_position);
            }
            pos.position = target_position;
            pos.history.push_back(std::make_tuple(timestamp, target_position, price));
        }
    }
    
    // Update capital weight and unrealized P&L
    pos.capital_weight = std::abs(pos.position * price * multiplier) / initial_capital;
    pos.unrealized_pnl = (price - pos.avg_price) * pos.position * multiplier;
}

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
        std::vector<std::string> all_symbols = db.getAllSymbols();  // Get all symbols from database
        std::cout << "\nTrading " << all_symbols.size() << " symbols:" << std::endl;
        for (const auto& symbol : all_symbols) {
            std::cout << symbol << " ";
        }
        std::cout << std::endl;

        // Initialize portfolio tracking
        double current_capital = initial_capital;
        
        // Initialize strategy with successful configuration
        auto strategy = std::make_unique<TrendStrategy>();
        
        // Configure strategy parameters
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
                
                // Calculate volatility-scaled position size
                double daily_return = (price / prev_price) - 1.0;
                double vol_scalar = 0.20 / (std::abs(daily_return) * std::sqrt(252.0) + 1e-10);  // Target 20% vol
                
                // Contract-specific position limits
                double max_position_multiplier = 1.0;
                if (symbol.substr(0, 2) == "6J" || symbol.substr(0, 2) == "6E" || 
                    symbol.substr(0, 2) == "6B" || symbol.substr(0, 2) == "6C") {
                    max_position_multiplier = 0.3;  // Reduce currency futures position sizes by 70%
                    vol_scalar *= 0.7;  // Less aggressive volatility scaling for currencies
                } else if (symbol.substr(0, 2) == "CL") {
                    max_position_multiplier = 1.5;  // Allow 50% larger positions for CL
                }
                
                // Scale position by volatility and apply reasonable contract size limits
                double capital_per_symbol = current_capital / all_symbols.size();
                double notional_position = signal * capital_per_symbol * vol_scalar;
                double max_contracts = capital_per_symbol * 0.1 * max_position_multiplier / price;
                double target_position = std::max(std::min(notional_position / price, max_contracts), -max_contracts);
                double position_change = target_position - positions[symbol].position;
                
                // Execute mock trade if position change is significant
                if (std::abs(position_change) > 0.01) {  // Minimum 0.01 contract size change
                    // Update position and check for exits first
                    updatePosition(symbol, signal, price, market_data[i].timestamp);
                    
                    // Mock execution for logging only
                    if (std::abs(position_change) > 0.01) {
                        ib.placeOrder(symbol, position_change, price, position_change > 0);
                    }
                }
                
                // Update unrealized P&L
                positions[symbol].unrealized_pnl = positions[symbol].position * (price - positions[symbol].avg_price);
            }

            // Print interim report after each symbol
            // Convert unordered_map to map for printing
            std::map<std::string, SymbolPosition> ordered_positions(positions.begin(), positions.end());
            printPortfolioReport(ordered_positions, initial_capital);
            
            // Sleep briefly to simulate real-time trading
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Print final portfolio report
        std::cout << "\nFinal Portfolio Report:" << std::endl;
        std::cout << "======================" << std::endl;
        // Convert unordered_map to map for printing
        std::map<std::string, SymbolPosition> ordered_positions(positions.begin(), positions.end());
        printPortfolioReport(ordered_positions, initial_capital);

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
} 