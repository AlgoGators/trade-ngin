#include <gtest/gtest.h>
#include "../trade_ngin/system/mock_ib_interface.hpp"
#include "../trade_ngin/system/test_trend_strategy.hpp"
#include "../trade_ngin/system/portfolio.hpp"
#include "../trade_ngin/data/data_interface.hpp"
#include "../trade_ngin/data/database_client.hpp"
#include <memory>
#include <random>

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize components
        auto db_client = std::make_shared<DatabaseClient>("postgresql://postgres:algogators@3.140.200.228:5432/algo_data");
        data_interface = std::make_unique<DataInterface>(db_client);
        strategy = std::make_unique<TrendStrategy>(100000.0, 0.15, 0.05, 0.30, 2.0);
        portfolio = std::make_unique<Portfolio>(100000.0); // Start with 100k capital
        mock_ib = std::make_unique<MockIBInterface>();

        // Setup mock data for test symbols
        setupMockData();
    }

    void setupMockData() {
        std::vector<std::string> test_symbols = {"GC.c.0", "CL.c.0", "ZW.c.0"};
        for (const auto& symbol : test_symbols) {
            mock_ib->addSymbol(symbol);
            mock_ib->setPrice(symbol, getInitialPrice(symbol));
        }
    }

    double getInitialPrice(const std::string& symbol) {
        if (symbol == "GC.c.0") return 1900.0;  // Gold
        if (symbol == "CL.c.0") return 75.0;    // Crude Oil
        if (symbol == "ZW.c.0") return 600.0;   // Wheat
        return 100.0;  // Default price
    }

    std::unique_ptr<DataInterface> data_interface;
    std::unique_ptr<TrendStrategy> strategy;
    std::unique_ptr<Portfolio> portfolio;
    std::unique_ptr<MockIBInterface> mock_ib;
};

TEST_F(IntegrationTest, TestFullTradingCycle) {
    // Get list of symbols to trade
    auto symbols = mock_ib->getAvailableSymbols();
    ASSERT_FALSE(symbols.empty());

    // Set risk management parameters
    for (const auto& symbol : symbols) {
        portfolio->setPositionLimit(symbol, 5.0);  // Maximum position size of 5 contracts
    }
    portfolio->setMaxDrawdown(0.1);  // 10% maximum drawdown limit

    // Random number generator for price changes with trend
    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<> price_change_dist(0.001, 0.015);  // Slight upward bias

    // Initialize price history for each symbol
    std::unordered_map<std::string, std::vector<MarketData>> price_history;
    for (const auto& symbol : symbols) {
        price_history[symbol] = std::vector<MarketData>();
    }

    // Simulate 30 days of trading
    for (int day = 0; day < 30; ++day) {
        std::cout << "\nDay " << day + 1 << ":\n";

        // Process each symbol
        for (const auto& symbol : symbols) {
            // Get current market data
            double current_price = mock_ib->getPrice(symbol);
            MarketData data;
            data.symbol = symbol;
            auto now = std::chrono::system_clock::now();
            auto now_time_t = std::chrono::system_clock::to_time_t(now);
            data.timestamp = std::ctime(&now_time_t);
            data.open = current_price;
            data.high = current_price * 1.01;
            data.low = current_price * 0.99;
            data.close = current_price;
            data.volume = 1000;

            // Add to price history
            price_history[symbol].push_back(data);

            // Generate trading signals if we have enough history
            if (price_history[symbol].size() >= 10) {  // Need minimum history for signals
                auto signals = strategy->generateSignals(price_history[symbol]);
                
                if (!signals.empty()) {
                    double signal = signals.back();  // Use most recent signal
                    
                    // Execute trades based on signals with conservative position sizing
                    if (std::abs(signal) > 0.2) {  // Increased threshold for more selective trading
                        bool is_buy = signal > 0;
                        
                        // Calculate position size based on risk and capital
                        double risk_per_trade = portfolio->getCurrentCapital() * 0.01;  // Risk 1% per trade
                        double price_volatility = std::max(data.high - data.low, current_price * 0.01);  // Use daily range or 1%
                        double quantity = risk_per_trade / (price_volatility * current_price);
                        quantity = std::min(quantity, 2.0);  // Cap position size at 2 contracts
                        
                        try {
                            // Place order and update portfolio
                            mock_ib->placeOrder(symbol, quantity, current_price, is_buy);
                            portfolio->processSignal(data, signal > 0 ? quantity : -quantity);
                            
                            std::cout << "Trade executed for " << symbol 
                                    << ": " << (is_buy ? "BUY" : "SELL")
                                    << " " << quantity << " @ " << current_price << "\n";
                        } catch (const std::runtime_error& e) {
                            std::cout << "Trade rejected: " << e.what() << "\n";
                        }
                    }
                }
            }

            // Simulate realistic price movement for next day
            double price_change = price_change_dist(gen);
            double new_price = current_price * (1.0 + price_change);
            mock_ib->setPrice(symbol, new_price);
        }

        // Update portfolio metrics
        portfolio->processSignal(price_history[symbols[0]].back(), 0.0);  // Update metrics with latest data
    }

    // Print detailed trading summary
    std::cout << "\nTrading Summary:\n";
    std::cout << "Initial Capital: " << 100000.0 << "\n"
              << "Final Capital: " << portfolio->getCurrentCapital() << "\n"
              << "Total Return: " << (portfolio->getTotalReturn() * 100.0) << "%\n"
              << "Annualized Return: " << (portfolio->getAnnualizedReturn() * 100.0) << "%\n"
              << "Sharpe Ratio: " << portfolio->getSharpeRatio() << "\n"
              << "Win Rate: " << (portfolio->getWinRate() * 100.0) << "%\n"
              << "Total Trades: " << portfolio->getTotalTrades() << "\n"
              << "Winning Trades: " << portfolio->getWinningTrades() << "\n"
              << "Profit Factor: " << portfolio->getProfitFactor() << "\n"
              << "Max Drawdown: " << (portfolio->getMaxDrawdown() * 100.0) << "%\n";

    // Verify trading activity and performance
    EXPECT_GT(portfolio->getTotalTrades(), 0);
    EXPECT_GT(portfolio->getCurrentCapital(), 0.0);
    EXPECT_GT(portfolio->getTotalReturn(), -0.5);  // Shouldn't lose more than 50%
    EXPECT_GT(portfolio->getWinRate(), 0.0);
    EXPECT_GT(portfolio->getProfitFactor(), 0.0);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
} 