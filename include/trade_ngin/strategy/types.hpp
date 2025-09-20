// include/trade_ngin/strategy/types.hpp
#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>
#include "trade_ngin/core/config_base.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Strategy state enumeration
 */
enum class StrategyState { INITIALIZED, RUNNING, PAUSED, STOPPED, ERROR };

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
struct StrategyConfig : public ConfigBase {
    // Basic parameters
    double capital_allocation{0.0};                           // Amount of capital allocated
    double max_leverage{0.0};                                 // Maximum leverage allowed
    std::unordered_map<std::string, double> position_limits;  // Per-symbol position limits

    // Risk parameters
    double max_drawdown{0.0};       // Maximum drawdown allowed
    double var_limit{0.0};          // Value at Risk limit
    double correlation_limit{0.0};  // Maximum correlation with other strategies

    // Trading parameters
    std::unordered_map<std::string, double> trading_params;  // Strategy-specific parameters
    std::unordered_map<std::string, double> costs;           // Trading costs per symbol

    // Data parameters
    std::vector<AssetClass> asset_classes;   // Asset classes to trade
    std::vector<DataFrequency> frequencies;  // Data frequencies to use

    // Persistence
    bool save_executions{false};  // Whether to save signals to database
    bool save_signals{false};     // Whether to save signals to database
    bool save_positions{false};   // Whether to save positions to database
    std::string signals_table;    // Table name for signals
    std::string positions_table;  // Table name for positions

    // Configuration metadata
    std::string version{"1.0.0"};  // Config version for migration

    // Implement serialization methods
    nlohmann::json to_json() const override {
        nlohmann::json j;
        j["capital_allocation"] = capital_allocation;
        j["max_leverage"] = max_leverage;
        j["position_limits"] = position_limits;
        j["max_drawdown"] = max_drawdown;
        j["var_limit"] = var_limit;
        j["correlation_limit"] = correlation_limit;
        j["trading_params"] = trading_params;
        j["costs"] = costs;

        // Convert enum vectors to string vectors for better readability
        j["asset_classes"] = nlohmann::json::array();
        for (const auto& asset_class : asset_classes) {
            j["asset_classes"].push_back(static_cast<int>(asset_class));
        }

        j["frequencies"] = nlohmann::json::array();
        for (const auto& freq : frequencies) {
            j["frequencies"].push_back(static_cast<int>(freq));
        }

        j["save_executions"] = save_executions;
        j["save_signals"] = save_signals;
        j["save_positions"] = save_positions;
        j["signals_table"] = signals_table;
        j["positions_table"] = positions_table;
        j["version"] = version;

        return j;
    }

    void from_json(const nlohmann::json& j) override {
        if (j.contains("capital_allocation"))
            capital_allocation = j.at("capital_allocation").get<double>();
        if (j.contains("max_leverage"))
            max_leverage = j.at("max_leverage").get<double>();
        if (j.contains("position_limits"))
            position_limits =
                j.at("position_limits").get<std::unordered_map<std::string, double>>();
        if (j.contains("max_drawdown"))
            max_drawdown = j.at("max_drawdown").get<double>();
        if (j.contains("var_limit"))
            var_limit = j.at("var_limit").get<double>();
        if (j.contains("correlation_limit"))
            correlation_limit = j.at("correlation_limit").get<double>();
        if (j.contains("trading_params"))
            trading_params = j.at("trading_params").get<std::unordered_map<std::string, double>>();
        if (j.contains("costs"))
            costs = j.at("costs").get<std::unordered_map<std::string, double>>();

        // Convert string arrays back to enum vectors
        if (j.contains("asset_classes")) {
            asset_classes.clear();
            for (const auto& asset_class : j.at("asset_classes")) {
                asset_classes.push_back(static_cast<AssetClass>(asset_class.get<int>()));
            }
        }

        if (j.contains("frequencies")) {
            frequencies.clear();
            for (const auto& freq : j.at("frequencies")) {
                frequencies.push_back(static_cast<DataFrequency>(freq.get<int>()));
            }
        }

        if (j.contains("save_executions"))
            save_executions = j.at("save_executions").get<bool>();
        if (j.contains("save_signals"))
            save_signals = j.at("save_signals").get<bool>();
        if (j.contains("save_positions"))
            save_positions = j.at("save_positions").get<bool>();
        if (j.contains("signals_table"))
            signals_table = j.at("signals_table").get<std::string>();
        if (j.contains("positions_table"))
            positions_table = j.at("positions_table").get<std::string>();
        if (j.contains("version"))
            version = j.at("version").get<std::string>();
    }
};

/**
 * @brief Performance metrics for a strategy
 */
struct StrategyMetrics {
    double unrealized_pnl;      // Total unrealized profit/loss
    double realized_pnl;        // Total realized profit/loss
    double total_pnl;           // Total profit/loss
    double sharpe_ratio;        // Sharpe ratio
    double sortino_ratio;       // Sortino ratio
    double max_drawdown;        // Maximum drawdown
    double win_rate;            // Win rate
    double profit_factor;       // Profit factor
    int total_trades;           // Total number of trades
    double avg_trade;           // Average profit per trade
    double avg_winner;          // Average winning trade
    double avg_loser;           // Average losing trade
    double max_winner;          // Largest winning trade
    double max_loser;           // Largest losing trade
    double avg_holding_period;  // Average holding period
    double turnover;            // Portfolio turnover
    double volatility;          // Portfolio volatility
};

}  // namespace trade_ngin
