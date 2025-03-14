#include "real_ibkr_interface.hpp"
#include "http_request.hpp"
#include "Logger.hpp"
#include <curl/curl.h>
#include <sstream>
#include <thread>
#include <random>

// Callback for libcurl to write response data
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

RealIBKRInterface::RealIBKRInterface(const IBKRConfig& config)
    : config_(config),
      connected_(false)
{
    // Initialize logging
    Logger::getInstance().info("Initializing IBKR interface with account: {}", config.accountId);
    
    // Initialize rate limits
    rateLimits_["/iserver/auth/status"] = {30, 0, std::chrono::steady_clock::now()};
    rateLimits_["/iserver/marketdata/snapshot"] = {50, 0, std::chrono::steady_clock::now()};
    rateLimits_["/iserver/account/trades"] = {60, 0, std::chrono::steady_clock::now()};
}

bool RealIBKRInterface::initializeSession() {
    try {
        // Step 1: Authenticate
        json authRequest = {
            {"accountId", config_.accountId}
        };
        
        auto response = performRequestWithRetry("POST", "/iserver/authenticate", authRequest);
        
        if (!response.contains("authenticated") || !response["authenticated"].get<bool>()) {
            Logger::getInstance().error("Authentication failed");
            return false;
        }
        
        // Step 2: Get session ID
        response = performRequestWithRetry("GET", "/iserver/auth/status");
        if (response.contains("session") && response["session"].get<std::string>() != "") {
            sessionId_ = response["session"].get<std::string>();
            connected_ = true;
            
            // Step 3: Start websocket for real-time data
            startWebSocket();
            
            Logger::getInstance().info("Session initialized successfully");
            return true;
        }
        
        Logger::getInstance().error("Failed to get session ID");
        return false;
        
    } catch (const std::exception& e) {
        Logger::getInstance().error("Failed to initialize session: {}", e.what());
        return false;
    }
}

bool RealIBKRInterface::subscribeMarketData(const std::string& symbol, 
                                          const std::vector<std::string>& fields) {
    std::lock_guard<std::mutex> lock(subscriptionMutex_);
    
    try {
        json request = {
            {"symbol", symbol},
            {"fields", fields}
        };
        
        auto response = performRequestWithRetry("POST", "/iserver/marketdata/snapshot", request);
        
        if (response.contains("conid")) {
            marketDataSubscriptions_[symbol] = response["conid"].get<int>();
            return true;
        }
        
        return false;
    } catch (const std::exception& e) {
        Logger::getInstance().error("Failed to subscribe to market data for {}: {}", 
                                  symbol, e.what());
        return false;
    }
}

json RealIBKRInterface::submitOrder(const Order& order) {
    // Validate order
    if (order.symbol.empty() || order.quantity <= 0) {
        throw std::invalid_argument("Invalid order parameters");
    }
    
    // Convert our order to IBKR format
    json orderRequest = {
        {"conid", order.conid},
        {"secType", "STK"},  // Default to stocks for now
        {"side", order.side},
        {"orderType", order.orderType},
        {"quantity", order.quantity},
        {"tif", order.tif},
        {"outsideRTH", order.outsideRth}
    };
    
    // Add price for limit orders
    if (order.orderType == "LMT" || order.orderType == "STP_LMT") {
        orderRequest["price"] = order.price;
    }
    
    // Submit order
    auto response = performRequestWithRetry(
        "POST", 
        "/iserver/account/" + config_.accountId + "/orders",
        orderRequest
    );
    
    // Update order callback if set
    if (orderCb_) {
        Order updatedOrder = order;
        updatedOrder.orderId = response["orderId"].get<std::string>();
        updatedOrder.status = "Submitted";
        orderCb_(updatedOrder);
    }
    
    return response;
}

void RealIBKRInterface::processWebSocketMessage(const std::string& message) {
    try {
        auto data = json::parse(message);
        
        // Handle different message types
        if (data.contains("messageType")) {
            std::string msgType = data["messageType"].get<std::string>();
            
            if (msgType == "marketData") {
                // Convert to our MarketData structure
                MarketData md;
                md.symbol = data["symbol"].get<std::string>();
                md.timestamp = data["timestamp"].get<std::string>();
                md.price = data["price"].get<double>();
                md.volume = data["size"].get<double>();
                
                if (marketDataCb_) {
                    marketDataCb_(md);
                }
            }
            else if (msgType == "orderStatus") {
                // Update order status
                if (orderCb_) {
                    Order order;
                    order.orderId = data["orderId"].get<std::string>();
                    order.status = data["status"].get<std::string>();
                    order.filled = data["filled"].get<double>();
                    order.avgFillPrice = data["avgPrice"].get<double>();
                    orderCb_(order);
                }
            }
            else if (msgType == "position") {
                // Update position
                if (positionCb_) {
                    Position pos;
                    pos.symbol = data["symbol"].get<std::string>();
                    pos.size = data["position"].get<double>();
                    pos.price = data["avgPrice"].get<double>();
                    positionCb_(pos);
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::getInstance().error("Failed to process websocket message: {}", e.what());
    }
}