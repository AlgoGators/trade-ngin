#pragma once
#include <vector>
#include <memory>
#include "portfolio.hpp"
#include "strategy.hpp"
#include "pnl.hpp"

class RiskEngine {
public:
    struct RiskMetrics {
        double var;              // Value at Risk
        double cvar;            // Conditional VaR
        double beta;            // Portfolio Beta
        double sharpe;          // Sharpe Ratio
        double sortino;         // Sortino Ratio
        double max_drawdown;    // Maximum Drawdown
        double correlation;     // Correlation Matrix
        double leverage;        // Current Leverage
        double stress_var;      // Stress Test VaR
    };

    RiskEngine(double confidence_level = 0.99, int lookback_days = 252)
        : confidence_level_(confidence_level), lookback_days_(lookback_days) {}

    RiskMetrics calculateRisk(const Portfolio& portfolio) {
        RiskMetrics metrics;
        
        // Get portfolio data
        auto positions = portfolio.getPositions();
        auto prices = portfolio.getPrices();
        auto pnl = portfolio.getPnL();
        
        // Calculate risk metrics
        metrics.var = calculateVaR(positions, prices);
        metrics.cvar = calculateCVaR(positions, prices);
        metrics.leverage = calculateLeverage(positions, prices);
        
        // Get performance metrics
        auto perf = pnl.getMetrics();
        metrics.sharpe = perf.sharpe_ratio;
        metrics.sortino = perf.sortino_ratio;
        metrics.max_drawdown = perf.max_drawdown;
        
        return metrics;
    }

    void runStressTest(const Portfolio& portfolio) {
        // Implement stress testing...
    }

    void setRiskLimits(const RiskMetrics& limits) {
        risk_limits_ = limits;
    }

private:
    double confidence_level_;
    int lookback_days_;
    RiskMetrics risk_limits_;

    double calculateVaR(const DataFrame& positions, const DataFrame& prices);
    double calculateCVaR(const DataFrame& positions, const DataFrame& prices);
    double calculateLeverage(const DataFrame& positions, const DataFrame& prices);
}; 