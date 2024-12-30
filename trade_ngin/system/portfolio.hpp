#pragma once
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include "dataframe.hpp"
#include "instrument.hpp"
#include "strategy.hpp"
#include "risk_measure.hpp"
#include "pnl.hpp"
#include "risk_engine.hpp"

class Portfolio {
public:
    struct PortfolioConfig {
        double initial_capital;
        double max_leverage;
        double margin_requirement;
        std::unordered_map<std::string, double> position_limits;
        std::unordered_map<std::string, double> risk_limits;
    };

    Portfolio(PortfolioConfig config);

    // Setup methods
    void addInstrument(std::shared_ptr<Instrument> instrument);
    void addStrategy(std::shared_ptr<Strategy> strategy, double weight);
    void setRiskEngine(std::shared_ptr<RiskEngine> risk_engine);
    void addPortfolioRule(std::function<void(Portfolio&)> rule);

    // Core portfolio methods
    void update();  // Update portfolio state
    void rebalance();  // Rebalance positions
    void executeOrders();  // Execute pending orders
    
    // Risk management
    void checkRiskLimits();
    void applyStressTests();
    void adjustPositions();
    
    // Position and exposure calculations
    DataFrame getMultipliers() const;
    DataFrame getPrices() const;
    DataFrame getPositions() const;
    DataFrame getExposure() const;
    
    // Risk and PnL
    PnL getPnL() const;
    RiskEngine::RiskMetrics getRiskMetrics() const;
    double getCapitalUtilization() const;

    // Portfolio metrics
    double getTotalValue() const;
    double getMarginUsage() const;
    std::vector<double> getStrategyWeights() const;
    
    // Event handlers
    void onOrderFill(const Order& order);
    void onMarketData(const MarketData& data);

private:
    PortfolioConfig config_;
    std::vector<std::shared_ptr<Instrument>> instruments_;
    std::vector<std::pair<double, std::shared_ptr<Strategy>>> weighted_strategies_;
    std::shared_ptr<RiskEngine> risk_engine_;
    std::vector<std::function<void(Portfolio&)>> portfolio_rules_;
    
    // Cache
    mutable std::optional<DataFrame> multipliers_;
    mutable std::optional<DataFrame> prices_;
    mutable std::optional<DataFrame> positions_;
    mutable std::optional<DataFrame> exposure_;
    
    // Helper methods
    void validateWeights() const;
    void applyPositionLimits();
    void applyRiskLimits();
    void updateCache();
};
