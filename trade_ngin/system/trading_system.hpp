#pragma once
#include <memory>
#include "portfolio.hpp"
#include "risk_engine.hpp"
#include "execution_engine.hpp"
#include "data_client.hpp"

class TradingSystem {
public:
    TradingSystem(double initial_capital, std::shared_ptr<DataClient> data_client)
        : portfolio_(Portfolio::PortfolioConfig{
              .initial_capital = initial_capital,
              .max_leverage = 2.0,
              .margin_requirement = 0.5
          }),
          data_client_(data_client),
          risk_engine_(std::make_shared<RiskEngine>()),
          execution_engine_(std::make_shared<ExecutionEngine>(
              std::make_shared<OrderManager>()
          )) {
        portfolio_.setRiskEngine(risk_engine_);
    }

    void addInstrument(std::unique_ptr<Instrument> instrument) {
        portfolio_.addInstrument(std::move(instrument));
    }

    void addStrategy(std::unique_ptr<Strategy> strategy, double weight) {
        portfolio_.addStrategy(std::move(strategy), weight);
    }

    void update() {
        // Update market data
        portfolio_.update();
        
        // Check risk limits before executing
        auto risk_metrics = risk_engine_->calculateRisk(portfolio_);
        if (risk_metrics.leverage > portfolio_.getConfig().max_leverage ||
            risk_metrics.var > portfolio_.getConfig().risk_limits["VAR"]) {
            portfolio_.adjustPositions(risk_metrics);
        }
        
        // Execute trades if within limits
        portfolio_.rebalance();
    }

    void execute() {
        portfolio_.rebalance();
    }

    const Portfolio& getPortfolio() const { return portfolio_; }
    PnL getPnL() const { return portfolio_.getPnL(); }

private:
    Portfolio portfolio_;
    std::shared_ptr<DataClient> data_client_;
    std::shared_ptr<RiskEngine> risk_engine_;
    std::shared_ptr<ExecutionEngine> execution_engine_;
}; 