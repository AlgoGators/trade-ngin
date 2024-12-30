#include "trading_system.hpp"
#include "volatility_risk.hpp"
#include <memory>

int main() {
    try {
        // Initialize core components
        auto data_client = std::make_shared<DatabentoClient>("API_KEY");
        auto risk_engine = std::make_shared<RiskEngine>();
        auto execution_engine = std::make_shared<ExecutionEngine>();
        
        // Create portfolio with config
        Portfolio::PortfolioConfig config{
            .initial_capital = 1000000.0,
            .max_leverage = 2.0,
            .margin_requirement = 0.5,
            .position_limits = {{"ES", 100}, {"NQ", 50}},
            .risk_limits = {{"VAR", 0.02}, {"MaxDrawdown", 0.1}}
        };
        
        auto portfolio = std::make_shared<Portfolio>(config);
        
        // Add instruments with proper handlers
        auto es_future = std::make_shared<Future>("ES", Dataset::CME, 50.0);
        es_future->addDataHandler(std::make_shared<VolatilityHandler>());
        es_future->addSignalProcessor(std::make_shared<signals::EMACrossover>(10, 30));
        portfolio->addInstrument(es_future);
        
        // Add strategies
        auto trend_strategy = std::make_shared<TrendFollowingStrategy>(
            500000.0, 50.0, 0.2, 1.0, 2.5
        );
        portfolio->addStrategy(trend_strategy, 0.7);
        
        // Main trading loop
        while (true) {
            portfolio->update();  // Updates instruments and strategies
            portfolio->rebalance();  // Generates and executes orders
            
            // Monitor and report
            auto metrics = portfolio->getPnL().getMetrics();
            auto risk = risk_engine->calculateRisk(*portfolio);
            
            // Log performance
            std::cout << "Sharpe: " << metrics.sharpe_ratio 
                      << " DrawDown: " << metrics.max_drawdown << "\n";
            
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
