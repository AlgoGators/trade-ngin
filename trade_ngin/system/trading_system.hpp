#pragma once
#include <memory>
#include "portfolio.hpp"
#include "risk_engine.hpp"
#include "execution_engine.hpp"
#include "data_client.hpp"
#include "database_interface.hpp"

class TradingSystem {
public:
    TradingSystem(double initial_capital, 
                  const std::string& databento_api_key)
        : portfolio_(Portfolio::PortfolioConfig{
              .initial_capital = initial_capital,
              .max_leverage = 2.0,
              .margin_requirement = 0.5
          }),
          data_client_(std::make_shared<DatabentoClient>(databento_api_key)),
          db_(std::make_unique<DatabaseInterface>()),
          risk_engine_(std::make_shared<RiskEngine>()),
          execution_engine_(std::make_shared<ExecutionEngine>(
              std::make_shared<OrderManager>()
          )) {
        portfolio_.setRiskEngine(risk_engine_);
        portfolio_.setDataClient(data_client_);
    }

    void initialize() {
        // Load available symbols from database
        auto symbols_table = db_->getSymbolsAsArrowTable();
        for (const auto& symbol : extractSymbols(symbols_table)) {
            auto instrument = std::make_unique<Future>(
                symbol, Dataset::CME, 50.0  // Example multiplier
            );
            portfolio_.addInstrument(std::move(instrument));
        }
    }

    void run() {
        while (running_) {
            update();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

private:
    Portfolio portfolio_;
    std::shared_ptr<DataClient> data_client_;
    std::unique_ptr<DatabaseInterface> db_;
    std::shared_ptr<RiskEngine> risk_engine_;
    std::shared_ptr<ExecutionEngine> execution_engine_;
    bool running_ = true;
    
    std::vector<std::string> extractSymbols(std::shared_ptr<arrow::Table> table);
}; 