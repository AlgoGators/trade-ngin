#include "trading_system.hpp"
#include "volatility_risk.hpp"
#include <memory>

int main() {
    try {
        // Create data client
        auto data_client = std::make_shared<LocalDataClient>();
        
        // Initialize trading system
        TradingSystem system(1000000.0, data_client); // $1M initial capital
        
        // Add instruments
        system.addInstrument(std::make_unique<Future>("ES", Dataset::CME, 50.0));
        system.addInstrument(std::make_unique<Future>("NQ", Dataset::CME, 20.0));
        
        // Add strategies with weights
        system.addStrategy(std::make_unique<trendFollowing>(1000000.0, 50.0), 0.7);
        system.addStrategy(std::make_unique<BuyAndHoldStrategy>(1000000.0), 0.3);
        
        // Set risk measure
        system.setRiskMeasure(std::make_unique<VolatilityRisk>(0.15));
        
        // Initialize the system
        system.initialize();
        
        // Main trading loop
        while (true) {
            system.update();  // Update data and positions
            system.execute(); // Execute trades
            
            // Print PnL
            auto pnl = system.getPnL();
            std::cout << "Current PnL: " << pnl.cumulativeProfit() << "\n";
            std::cout << "Sharpe Ratio: " << pnl.sharpeRatio() << "\n";
            
            // Sleep for some time before next iteration
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
