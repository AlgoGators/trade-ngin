#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class PerformanceAnalytics {
public:
    struct TradeStats {
        // Basic metrics
        int total_trades;
        int winning_trades;
        int losing_trades;
        double win_rate;
        double avg_win;
        double avg_loss;
        double profit_factor;
        
        // Risk metrics
        double max_drawdown;
        double max_drawdown_duration;
        double var_95;
        double cvar_95;
        double omega_ratio;
        double sortino_ratio;
        double calmar_ratio;
        double information_ratio;
        
        // Return metrics
        double total_return;
        double annualized_return;
        double annualized_volatility;
        double sharpe_ratio;
        std::vector<double> monthly_returns;
        std::vector<double> daily_returns;
        
        // Position metrics
        double avg_position_size;
        double max_position_size;
        double avg_holding_period;
        double portfolio_beta;
        double alpha;
        
        // Risk allocation
        std::unordered_map<std::string, double> symbol_allocation;
        std::unordered_map<std::string, double> symbol_contribution;
        std::vector<double> rolling_var;
        std::vector<double> rolling_sharpe;
    };

    struct RealTimeMetrics {
        // Current portfolio state
        double current_equity;
        double cash_balance;
        double margin_used;
        double buying_power;
        
        // Risk exposure
        double current_var;
        double current_leverage;
        double net_exposure;
        double gross_exposure;
        
        // Active positions
        struct PositionMetrics {
            double quantity;
            double avg_price;
            double current_price;
            double unrealized_pnl;
            double realized_pnl;
            double position_var;
            double position_beta;
            std::vector<double> intraday_prices;
        };
        std::unordered_map<std::string, PositionMetrics> positions;
        
        // Trading activity
        struct TodayActivity {
            int trades_today;
            double today_pnl;
            double today_fees;
            double today_turnover;
            std::vector<std::pair<std::string, double>> top_winners;
            std::vector<std::pair<std::string, double>> top_losers;
        } today;
    };

    PerformanceAnalytics();

    // Update methods
    void updateTrade(const std::string& symbol, double quantity, double price, bool is_buy);
    void updatePosition(const std::string& symbol, const json& market_data);
    void updateEquity(double new_equity);
    void updateDailyStats();
    void updateRiskMetrics();

    // Analysis methods
    TradeStats getHistoricalStats() const;
    RealTimeMetrics getCurrentMetrics() const;
    json getDetailedAnalysis() const;

    // Risk analysis
    double calculateVaR(double confidence_level = 0.95) const;
    double calculateCVaR(double confidence_level = 0.95) const;
    std::vector<double> calculateRollingVaR(int window = 20) const;
    std::vector<double> calculateRollingSharpe(int window = 60) const;

    // Performance analysis
    double calculateSharpe(const std::vector<double>& returns) const;
    double calculateSortino(const std::vector<double>& returns) const;
    double calculateCalmar() const;
    double calculateOmega() const;
    std::vector<double> calculateDrawdowns() const;
    
    // Portfolio analysis
    std::unordered_map<std::string, double> calculateFactorExposures() const;
    std::vector<std::pair<std::string, double>> getTopContributors() const;
    json generatePerformanceReport() const;

private:
    // Historical data
    std::vector<double> equity_curve_;
    std::vector<double> daily_returns_;
    std::vector<double> monthly_returns_;
    std::unordered_map<std::string, std::vector<double>> symbol_returns_;
    
    // Current state
    RealTimeMetrics current_metrics_;
    TradeStats historical_stats_;
    
    // Trade history
    struct Trade {
        std::string symbol;
        double quantity;
        double entry_price;
        double exit_price;
        std::string entry_time;
        std::string exit_time;
        double pnl;
        double fees;
    };
    std::vector<Trade> trade_history_;
    
    // Helper methods
    double calculateVolatility(const std::vector<double>& returns) const;
    double calculateBeta(const std::vector<double>& returns, const std::vector<double>& benchmark) const;
    std::vector<double> calculateRollingMetric(const std::vector<double>& data, int window, 
        std::function<double(const std::vector<double>&)> metric_func) const;
};
