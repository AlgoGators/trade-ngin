#pragma once
#include <memory>
#include <vector>
#include "portfolio.hpp"
#include "risk_measure.hpp"
#include "strategy.h"
#include "instrument.hpp"
#include "data_client.hpp"

class TradingSystem {
public:
    TradingSystem(double initial_capital, std::shared_ptr<DataClient> data_client)
        : capital_(initial_capital), data_client_(data_client) {}

    // System setup methods
    void addInstrument(std::unique_ptr<Instrument> instrument);
    void addStrategy(std::unique_ptr<Strategy> strategy, double weight);
    void setRiskMeasure(std::unique_ptr<RiskMeasure> risk_measure);
    
    // Core trading operations
    void initialize();
    void update();
    void execute();
    
    // Getters
    const Portfolio& getPortfolio() const { return portfolio_; }
    PnL getPnL() const { return portfolio_.getPnL(); }

private:
    double capital_;
    std::shared_ptr<DataClient> data_client_;
    Portfolio portfolio_;
    std::vector<std::unique_ptr<Instrument>> instruments_;
    std::vector<std::pair<double, std::unique_ptr<Strategy>>> weighted_strategies_;
    std::unique_ptr<RiskMeasure> risk_measure_;
}; 