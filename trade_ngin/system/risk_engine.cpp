void RiskEngine::updateRiskMetrics(const Portfolio& portfolio) {
    auto pnl = portfolio.getPnL();
    auto metrics = pnl.getMetrics();
    
    // Update risk limits based on performance
    if (metrics.max_drawdown > risk_limits_.max_drawdown) {
        adjustRiskLimits(metrics);
        portfolio.notifyRiskLimitChange(risk_limits_);
    }
} 