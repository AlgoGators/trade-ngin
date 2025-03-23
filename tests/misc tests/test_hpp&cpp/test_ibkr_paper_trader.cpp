#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include "../system/ibkr_interface.hpp"
#include "mock_ibkr_server.hpp"

class IBKRPaperTraderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Drop any existing loggers
        spdlog::drop_all();
        
        // Start mock server
        server = std::make_unique<MockIBKRServer>(8080);
        server->start();
        
        // Wait for server to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Initialize IBKR interface with test config
        client = std::make_unique<IBKRInterface>("config/ibkr_config_test.json");

        // Setup mock data
        setupMockData();
    }

    void TearDown() override {
        server->stop();
        server.reset();
        client.reset();
    }

    void setupMockData() {
        // Setup mock market data
        json aapl_data = {
            {"symbol", "AAPL"},
            {"last", 150.25},
            {"bid", 150.20},
            {"ask", 150.30},
            {"volume", 1000000}
        };
        server->setMarketData("AAPL", aapl_data);

        // Setup mock historical data
        json historical = {
            {"symbol", "AAPL"},
            {"interval", "1d"},
            {"data", json::array({
                {
                    {"timestamp", "2024-02-20"},
                    {"open", 149.50},
                    {"high", 151.20},
                    {"low", 149.00},
                    {"close", 150.25},
                    {"volume", 1000000}
                }
            })}
        };
        server->setHistoricalData("AAPL", historical);

        // Setup mock account data
        json account = {
            {"account_id", "paper_account_123"},
            {"cash", 100000.0},
            {"buying_power", 200000.0},
            {"equity", 150000.0}
        };
        server->setAccountData(account);

        // Setup mock positions
        json positions = {
            {"positions", json::array({
                {
                    {"symbol", "AAPL"},
                    {"quantity", 100},
                    {"avg_price", 145.50},
                    {"market_value", 15025.0}
                }
            })}
        };
        server->setPositions(positions);
    }

    std::unique_ptr<MockIBKRServer> server;
    std::unique_ptr<IBKRInterface> client;
};

TEST_F(IBKRPaperTraderTest, TestAuthentication) {
    ASSERT_TRUE(client->authenticate());
    ASSERT_TRUE(client->isConnected());
}

TEST_F(IBKRPaperTraderTest, TestMarketData) {
    ASSERT_TRUE(client->authenticate());  // Ensure we're authenticated first
    auto data = client->getMarketData("AAPL", {"last", "bid", "ask"});
    ASSERT_FALSE(data.empty());
    ASSERT_EQ(data["symbol"], "AAPL");
    ASSERT_EQ(data["last"], 150.25);
}

TEST_F(IBKRPaperTraderTest, TestHistoricalData) {
    ASSERT_TRUE(client->authenticate());  // Ensure we're authenticated first
    auto data = client->getHistoricalData("AAPL", "1d", "1d");
    ASSERT_FALSE(data.empty());
    ASSERT_EQ(data["symbol"], "AAPL");
    ASSERT_FALSE(data["data"].empty());
}

TEST_F(IBKRPaperTraderTest, TestOrderPlacement) {
    ASSERT_TRUE(client->authenticate());  // Ensure we're authenticated first
    auto order = client->placeOrder("AAPL", 100, 150.0, true);
    ASSERT_FALSE(order.empty());
    ASSERT_EQ(order["status"], "submitted");
    ASSERT_FALSE(order["order_id"].empty());
}

TEST_F(IBKRPaperTraderTest, TestAccountData) {
    ASSERT_TRUE(client->authenticate());  // Ensure we're authenticated first
    auto data = client->getAccountSummary();
    ASSERT_FALSE(data.empty());
    ASSERT_EQ(data["account_id"], "paper_account_123");
    ASSERT_GT(data["cash"], 0);
}

TEST_F(IBKRPaperTraderTest, TestPositions) {
    ASSERT_TRUE(client->authenticate());  // Ensure we're authenticated first
    auto positions = client->getPositions();
    ASSERT_FALSE(positions.empty());
    ASSERT_FALSE(positions["positions"].empty());
    ASSERT_EQ(positions["positions"][0]["symbol"], "AAPL");
}

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
