// include/trade_ngin/strategy/types.hpp
#pragma once

#include "trade_ngin/core/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace trade_ngin {

/**
 * @brief Strategy state enumeration
 */
enum class StrategyState {
    INITIALIZED,
    RUNNING,
    PAUSED,
    STOPPED,
    ERROR
};

/**
 * @brief Strategy metadata structure
 */
struct StrategyMetadata {
    std::string id;                    // Unique strategy identifier
    std::string name;                  // Strategy name
    std::string description;           // Strategy description
    std::vector<AssetClass> assets;    // Supported asset classes
    std::vector<DataFrequency> freqs;  // Supported data frequencies
    double sharpe_ratio;               // Historical Sharpe ratio
    double sortino_ratio;              // Historical Sortino ratio
    double max_drawdown;               // Historical max drawdown
    double win_rate;                   // Historical win rate
};

/**
 * @brief Extended strategy configuration
 */
struct StrategyConfig {
    // Basic parameters
    double capital_allocation;         // Amount of capital allocated
    double max_leverage;              // Maximum leverage allowed
    std::unordered_map<std::string, double> position_limits;  // Per-symbol position limits
    
    // Risk parameters
    double max_drawdown;             // Maximum drawdown allowed
    double var_limit;                // Value at Risk limit
    double correlation_limit;        // Maximum correlation with other strategies
    
    // Trading parameters
    std::unordered_map<std::string, double> trading_params;  // Strategy-specific parameters
    std::unordered_map<std::string, double> costs;          // Trading costs per symbol
    
    // Data parameters
    std::vector<AssetClass> asset_classes;    // Asset classes to trade
    std::vector<DataFrequency> frequencies;   // Data frequencies to use
    
    // Persistence
    bool save_executions;               // Whether to save signals to database
    bool save_signals;               // Whether to save signals to database
    bool save_positions;             // Whether to save positions to database
    std::string signals_table;       // Table name for signals
    std::string positions_table;     // Table name for positions
};

/**
 * @brief Performance metrics for a strategy
 */
struct StrategyMetrics {
    double total_pnl;                // Total profit/loss
    double sharpe_ratio;             // Sharpe ratio
    double sortino_ratio;            // Sortino ratio
    double max_drawdown;             // Maximum drawdown
    double win_rate;                 // Win rate
    double profit_factor;            // Profit factor
    int total_trades;                // Total number of trades
    double avg_trade;                // Average profit per trade
    double avg_winner;               // Average winning trade
    double avg_loser;                // Average losing trade
    double max_winner;               // Largest winning trade
    double max_loser;                // Largest losing trade
    double avg_holding_period;       // Average holding period
    double turnover;                 // Portfolio turnover
    double volatility;               // Portfolio volatility
};

} // namespace trade_ngin