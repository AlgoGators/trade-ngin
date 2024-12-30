#pragma once
#include <vector>
#include "portfolio.hpp"

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

    RiskEngine(double confidence_level = 0.99, int lookback_days = 252);
    RiskMetrics calculateRisk(const Portfolio& portfolio);
    void runStressTest(const Portfolio& portfolio);
    void setRiskLimits(const RiskMetrics& limits);

private:
    double confidence_level_;
    int lookback_days_;
    RiskMetrics risk_limits_;
}; 