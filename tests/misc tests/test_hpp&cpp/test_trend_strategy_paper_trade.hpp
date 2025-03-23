#pragma once

#include <memory>
#include <vector>
#include <pqxx/pqxx>
#include "../system/ibkr_interface.hpp"
#include "../strategy/trend_strategy.hpp"
#include "../data/database_client.hpp"

class TrendStrategyPaperTrader {
public:
    struct TradeStats {
        int total_trades;
        int winning_trades;
        double total_pnl;
        double max_drawdown;
        double sharpe_ratio;
        std::vector<double> daily_returns;
        std::unordered_map<std::string, std::vector<double>> position_history;
    };

    TrendStrategyPaperTrader(
        std::shared_ptr<IBKRInterface> ibkr,
        std::shared_ptr<DatabaseClient> db_client,
        double initial_capital = 1000000.0,
        double risk_target = 0.2,
        double leverage_limit = 2.0
    ) : ibkr_(ibkr), db_client_(db_client), 
        initial_capital_(initial_capital),
        risk_target_(risk_target),
        leverage_limit_(leverage_limit) {
        setupStrategy();
    }

    // Initialize strategy with parameters
    void setupStrategy() {
        strategy_params_ = {
            {"ma_short", 10},
            {"ma_medium", 50},
            {"ma_long", 200},
            {"volatility_window", 20},
            {"momentum_window", 14},
            {"regime_window", 100}
        };
    }

    // Run paper trading simulation
    TradeStats runSimulation(
        const std::vector<std::string>& symbols,
        const std::string& start_date,
        const std::string& end_date,
        bool use_real_time = false
    );

    // Process single trading day
    void processTradingDay(
        const std::string& date,
        const std::vector<std::string>& symbols
    );

    // Generate trading signals
    std::unordered_map<std::string, double> generateSignals(
        const std::string& symbol,
        const json& market_data
    );

    // Position sizing and risk management
    double calculatePositionSize(
        const std::string& symbol,
        double signal_strength,
        const json& market_data
    );

    // Portfolio management
    void updatePortfolio(
        const std::unordered_map<std::string, double>& target_positions
    );

    // Performance tracking
    void updatePerformanceMetrics(const std::string& date);
    
    // Risk metrics calculation
    double calculateVolatility(const std::vector<double>& returns);
    double calculateSharpeRatio(const std::vector<double>& returns);
    double calculateMaxDrawdown(const std::vector<double>& equity_curve);

private:
    std::shared_ptr<IBKRInterface> ibkr_;
    std::shared_ptr<DatabaseClient> db_client_;
    std::unordered_map<std::string, double> strategy_params_;
    
    double initial_capital_;
    double current_capital_;
    double risk_target_;
    double leverage_limit_;
    
    std::unordered_map<std::string, double> current_positions_;
    std::vector<double> equity_curve_;
    TradeStats stats_;

    // Database helpers
    std::vector<json> fetchHistoricalData(
        const std::string& symbol,
        const std::string& start_date,
        const std::string& end_date
    );
};
