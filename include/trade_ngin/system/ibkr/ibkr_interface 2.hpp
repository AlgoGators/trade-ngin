#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <curl/curl.h>

using json = nlohmann::json;

class IBKRInterface {
public:
    // Constructor initializes CURL and sets up logging
    IBKRInterface(const std::string& api_endpoint, const std::string& account_id);
    ~IBKRInterface();

    // Session management
    bool authenticate();
    bool validateSession();
    bool isConnected() const;

    // Market data operations
    json getMarketData(const std::string& symbol, const std::vector<std::string>& fields);
    json getHistoricalData(const std::string& symbol, const std::string& duration, const std::string& bar_size);
    
    // Trading operations
    json placeOrder(const std::string& symbol, double quantity, double price, bool is_buy);
    json modifyOrder(const std::string& orderId, double quantity, double price);
    json cancelOrder(const std::string& orderId);
    
    // Account operations
    json getAccountSummary();
    json getPositions();
    json getOpenOrders();

private:
    // HTTP request helpers
    json makeRequest(const std::string& endpoint, const std::string& method, const json& data = json());
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
    
    // Session management
    bool refreshSession();
    void setupCurlHandle();

    // Internal state
    CURL* curl;
    std::string base_url;
    std::string account_id;
    std::string session_id;
    bool connected;
    
    // Logging
    std::shared_ptr<spdlog::logger> logger;
    
    // Rate limiting
    struct RateLimit {
        int max_requests;
        int current_requests;
        std::chrono::steady_clock::time_point reset_time;
    };
    std::unordered_map<std::string, RateLimit> rate_limits;
    bool checkRateLimit(const std::string& endpoint);
    void updateRateLimit(const std::string& endpoint);
}; 