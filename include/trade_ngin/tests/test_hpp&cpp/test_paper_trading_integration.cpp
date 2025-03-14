#include <gtest/gtest.h>
#include <memory>
#include <chrono>
#include <thread>
#include "test_trend_strategy_paper_trade.hpp"
#include "mock_ibkr_server.hpp"
#include "../data/database_client.hpp"

class PaperTradingIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Start mock server
        server = std::make_unique<MockIBKRServer>(8080);
        server->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Initialize IBKR interface
        ibkr = std::make_shared<IBKRInterface>(
            "http://localhost:8080",
            "paper_account_123"
        );

        // Initialize database client
        db_client = std::make_shared<DatabaseClient>(
            "host=3.140.200.228 "
            "port=5432 "
            "dbname=algo_data "
            "user=postgres "
            "password=algogators"
        );

        // Setup mock market data
        setupMockData();
    }

    void TearDown() override {
        server->stop();
    }

    void setupMockData() {
        // Setup mock market data for multiple symbols
        // Use futures contracts
        std::vector<std::string> symbols = {"6E.c.0", "6A.c.0", "6B.c.0"};
        
        for (const auto& symbol : symbols) {
            // Get latest data from database for realistic prices
            auto latest_data = db_client->executeQuery(
                "SELECT close, volume FROM futures_data.ohlcv_1d "
                "WHERE symbol = '" + symbol + "' ORDER BY time DESC LIMIT 1"
            );

            double last_price = latest_data[0]["close"].as<double>();
            int last_volume = latest_data[0]["volume"].as<int>();

            // Current market data with realistic prices
            json market_data = {
                {"symbol", symbol},
                {"last", last_price},
                {"bid", last_price - 0.0001},  // 1 pip spread
                {"ask", last_price + 0.0001},
                {"volume", last_volume},
                {"high", 151.00},
                {"low", 149.50}
            };
            server->setMarketData(symbol, market_data);

            // Historical data
            json historical = {
                {"symbol", symbol},
                {"interval", "1d"},
                {"data", json::array()}
            };

            // Generate 252 days of mock historical data
            double price = 150.0;
            for (int i = 0; i < 252; ++i) {
                double change = (rand() % 200 - 100) / 1000.0; // Random price change
                price *= (1 + change);
                
                json bar = {
                    {"date", "2024-02-" + std::to_string(20 - i)},
                    {"open", price * 0.99},
                    {"high", price * 1.02},
                    {"low", price * 0.98},
                    {"close", price},
                    {"volume", 1000000 + rand() % 1000000}
                };
                historical["data"].push_back(bar);
            }
            server->setHistoricalData(symbol, historical);
        }

        // Setup mock account data
        json account = {
            {"account_id", "paper_account_123"},
            {"cash", 1000000.0},
            {"buying_power", 2000000.0},
            {"equity", 1000000.0}
        };
        server->setAccountData(account);
    }

    std::unique_ptr<MockIBKRServer> server;
    std::shared_ptr<IBKRInterface> ibkr;
    std::shared_ptr<DatabaseClient> db_client;
};

TEST_F(PaperTradingIntegrationTest, TestBasicTradingStrategy) {
    TrendStrategyPaperTrader trader(ibkr, db_client);
    std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT"};
    
    auto stats = trader.runSimulation(
        symbols,
        "2024-01-01",
        "2024-02-20",
        false // Use historical data
    );
    
    ASSERT_GT(stats.total_trades, 0);
    ASSERT_GE(stats.winning_trades, 0);
    ASSERT_LE(stats.max_drawdown, 1.0);
}

TEST_F(PaperTradingIntegrationTest, TestRiskManagement) {
    TrendStrategyPaperTrader trader(
        ibkr, 
        db_client,
        1000000.0,  // Initial capital
        0.02,       // 2% risk per trade
        1.5         // Max leverage
    );
    
    std::vector<std::string> symbols = {"AAPL"};
    auto stats = trader.runSimulation(
        symbols,
        "2024-01-01",
        "2024-02-20",
        false
    );
    
    // Check position sizes are within limits
    for (const auto& [symbol, positions] : stats.position_history) {
        for (double pos : positions) {
            // Calculate position value
            json market_data = ibkr->getMarketData(symbol, {"last"});
            double position_value = std::abs(pos * market_data["last"].get<double>());
            
            // Check leverage limit
            ASSERT_LE(position_value, 1000000.0 * 1.5);
        }
    }
}

TEST_F(PaperTradingIntegrationTest, TestPortfolioRebalancing) {
    TrendStrategyPaperTrader trader(ibkr, db_client);
    std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT"};
    
    // Run simulation for one day
    auto stats = trader.runSimulation(
        symbols,
        "2024-02-20",
        "2024-02-20",
        true // Use real-time data
    );
    
    // Check that positions were taken
    ASSERT_FALSE(stats.position_history.empty());
    
    // Verify portfolio constraints
    double total_exposure = 0.0;
    for (const auto& [symbol, positions] : stats.position_history) {
        if (!positions.empty()) {
            json market_data = ibkr->getMarketData(symbol, {"last"});
            total_exposure += std::abs(positions.back() * market_data["last"].get<double>());
        }
    }
    
    // Check total exposure is within limits
    ASSERT_LE(total_exposure, 1000000.0 * 2.0);
}

TEST_F(PaperTradingIntegrationTest, TestSignalGeneration) {
    TrendStrategyPaperTrader trader(ibkr, db_client);
    
    // Get market data for signal generation
    json market_data = ibkr->getMarketData("AAPL", {"last", "volume", "high", "low"});
    
    // Generate signals
    auto signals = trader.generateSignals("AAPL", market_data);
    
    // Verify signal properties
    ASSERT_FALSE(signals.empty());
    for (const auto& [symbol, signal] : signals) {
        ASSERT_GE(signal, -1.0);
        ASSERT_LE(signal, 1.0);
    }
}

TEST_F(PaperTradingIntegrationTest, TestPerformanceMetrics) {
    TrendStrategyPaperTrader trader(ibkr, db_client);
    std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT"};
    
    auto stats = trader.runSimulation(
        symbols,
        "2024-01-01",
        "2024-02-20",
        false
    );
    
    // Verify performance metrics
    ASSERT_GE(stats.total_trades, 0);
    ASSERT_LE(stats.max_drawdown, 1.0);
    ASSERT_FALSE(stats.daily_returns.empty());
    
    // Calculate expected Sharpe ratio range
    double sharpe = stats.sharpe_ratio;
    ASSERT_FALSE(std::isnan(sharpe));
    ASSERT_GT(std::abs(sharpe), 0.0);
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
