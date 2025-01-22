#include "test_trend_strategy.hpp"
#include "../data/data_interface.hpp"
#include "../data/ohlcv_data_handler.hpp"
#include "mock_ib_interface.hpp"
#include <memory>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <map>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

struct SymbolPosition {
    std::string symbol;
    double position = 0.0;
    double signal = 0.0;
    double avg_price = 0.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    double pnl = 0.0;
    int trades = 0;
    int winning_trades = 0;
    int total_trades = 0;
    double capital_weight = 0.0;
    double avg_win = 0.0;
    double avg_loss = 0.0;
    double max_profit_trade = 0.0;
    double max_loss_trade = 0.0;
    double avg_hold_time_wins = 0.0;
    double avg_hold_time_losses = 0.0;
    std::string last_trade_time;
    bool in_position = false;
    std::vector<std::tuple<std::string, double, double>> history;

    SymbolPosition() = default;
    SymbolPosition(const std::string& s) : symbol(s) {}
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

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: " << argv[0] << " <connection_string>\n";
            return 1;
        }

        // Initialize data handler
        OHLCVDataHandler data_handler(argv[1]);

        // Create strategy with default parameters
        auto strategy = std::make_shared<TrendStrategy>(1000000.0, 0.15, 0.05, 0.30, 2.0);

        // Get list of symbols to trade
        auto symbols_table = data_handler.getSymbolsAsArrowTable();
        if (!symbols_table || symbols_table->num_rows() == 0) {
            throw std::runtime_error("No symbols found in database");
        }

        // Extract symbols from Arrow table
        auto symbols_array = std::static_pointer_cast<arrow::ChunkedArray>(symbols_table->column(0));
        std::vector<std::string> symbols;
        for (int chunk_idx = 0; chunk_idx < symbols_array->num_chunks(); ++chunk_idx) {
            auto chunk = std::static_pointer_cast<arrow::StringArray>(symbols_array->chunk(chunk_idx));
            for (int i = 0; i < chunk->length(); ++i) {
                symbols.push_back(chunk->GetString(i));
            }
        }

        // Initialize positions for each symbol
        std::vector<SymbolPosition> positions;
        positions.reserve(symbols.size());
        for (const auto& symbol : symbols) {
            positions.emplace_back(symbol);
        }

        // Get date range
        std::string start_date = data_handler.getEarliestDate();
        std::string end_date = data_handler.getLatestDate();

        // Process each symbol
        for (auto& pos : positions) {
            // Get OHLCV data
            auto data_table = data_handler.getOHLCVArrowTable(start_date, end_date, {pos.symbol});
            if (!data_table || data_table->num_rows() == 0) {
                continue;
            }

            // Convert Arrow table to vector of MarketData
            std::vector<MarketData> market_data;
            auto timestamp_array = std::static_pointer_cast<arrow::ChunkedArray>(data_table->GetColumnByName("timestamp"));
            auto open_array = std::static_pointer_cast<arrow::ChunkedArray>(data_table->GetColumnByName("open"));
            auto high_array = std::static_pointer_cast<arrow::ChunkedArray>(data_table->GetColumnByName("high"));
            auto low_array = std::static_pointer_cast<arrow::ChunkedArray>(data_table->GetColumnByName("low"));
            auto close_array = std::static_pointer_cast<arrow::ChunkedArray>(data_table->GetColumnByName("close"));
            auto volume_array = std::static_pointer_cast<arrow::ChunkedArray>(data_table->GetColumnByName("volume"));

            size_t num_rows = data_table->num_rows();
            market_data.reserve(num_rows);

            for (int64_t i = 0; i < num_rows; ++i) {
                MarketData bar;
                bar.symbol = pos.symbol;
                bar.timestamp = std::static_pointer_cast<arrow::StringArray>(timestamp_array->chunk(0))->GetString(i);
                bar.open = std::static_pointer_cast<arrow::DoubleArray>(open_array->chunk(0))->Value(i);
                bar.high = std::static_pointer_cast<arrow::DoubleArray>(high_array->chunk(0))->Value(i);
                bar.low = std::static_pointer_cast<arrow::DoubleArray>(low_array->chunk(0))->Value(i);
                bar.close = std::static_pointer_cast<arrow::DoubleArray>(close_array->chunk(0))->Value(i);
                bar.volume = std::static_pointer_cast<arrow::DoubleArray>(volume_array->chunk(0))->Value(i);
                market_data.push_back(bar);
            }

            // Generate signals
            auto signals = strategy->generateSignals(market_data);

            // Process signals and update metrics
            for (size_t i = 1; i < signals.size(); ++i) {
                if (signals[i] != signals[i-1]) {
                    // Update position metrics
                    if (pos.position != 0) {
                        double trade_pnl = pos.position * (market_data[i].close - pos.avg_price);
                        pos.pnl += trade_pnl;
                        pos.trades++;
                        if (trade_pnl > 0) {
                            pos.winning_trades++;
                        }
                    }
                    pos.position = signals[i];
                    pos.avg_price = market_data[i].close;
                }
            }

            // Print results for this symbol
            std::cout << "Symbol: " << pos.symbol << "\n";
            std::cout << "Final P&L: " << pos.pnl << "\n";
            std::cout << "Total Trades: " << pos.trades << "\n";
            std::cout << "Win Rate: " << (pos.trades > 0 ? (double)pos.winning_trades / pos.trades : 0.0) << "\n";
            std::cout << "-------------------\n";
        }

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
} 