#pragma once
#include "portfolio.hpp"
#include "risk_engine.hpp"

class Backtester {
public:
    struct BacktestConfig {
        std::chrono::system_clock::time_point start_date;
        std::chrono::system_clock::time_point end_date;
        double initial_capital;
        bool include_transaction_costs;
        bool include_slippage;
        bool include_market_impact;
    };

    struct BacktestResults {
        double total_return;
        double sharpe_ratio;
        double max_drawdown;
        double win_rate;
        double profit_factor;
        std::vector<Trade> trades;
        RiskEngine::RiskMetrics risk_metrics;
    };

    Backtester(const BacktestConfig& config);
    BacktestResults run(const Portfolio& portfolio);
    void generateReport(const std::string& output_path);

private:
    BacktestConfig config_;
    std::unique_ptr<RiskEngine> risk_engine_;
}; 