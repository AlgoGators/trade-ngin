#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

using json = nlohmann::json;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

class MockIBKRServer {
public:
    MockIBKRServer(int port = 8080);
    ~MockIBKRServer();

    // Server control
    void start();
    void stop();
    bool isRunning() const;

    // Mock data setup
    void setMarketData(const std::string& symbol, const json& data);
    void setHistoricalData(const std::string& symbol, const json& data);
    void setAccountData(const json& data);
    void setPositions(const json& data);

    // Mock response handlers
    void handleAuthentication(const json& request, json& response);
    void handleAuthStatus(const json& request, json& response);
    void handleMarketData(const json& request, json& response);
    void handleHistoricalData(const json& request, json& response);
    void handleOrder(const json& request, json& response);
    void handleAccountData(const json& request, json& response);

private:
    // Async server handling
    void startAccept();
    void handleConnection(std::shared_ptr<tcp::socket> socket);
    void processRequest(const json& request, std::string& response);

    // Internal state
    boost::asio::io_context io_context;
    std::unique_ptr<tcp::acceptor> acceptor;
    bool running;
    int port;

    // Mock data storage
    std::unordered_map<std::string, json> market_data;
    std::unordered_map<std::string, json> historical_data;
    json account_data;
    json positions_data;
    std::vector<json> order_history;

    // Session management
    std::unordered_map<std::string, std::string> active_sessions;
    std::string generateSessionId();

    // Logging
    std::shared_ptr<spdlog::logger> logger;
};
