#ifndef REAL_IBKR_INTERFACE_HPP
#define REAL_IBKR_INTERFACE_HPP

#include "ibkr_interface.hpp"
#include "../config/ibkr_config.hpp"
#include "market_data.hpp"
#include "portfolio.hpp"
#include <string>
#include <vector>
#include <chrono>
#include <queue>
#include <mutex>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Enhanced Order structure with IBKR-specific fields
struct Order {
    int conid;              // Instrument contract id from IBKR
    std::string symbol;     // Trading symbol
    std::string side;       // "BUY" or "SELL"
    std::string orderType;  // "MKT", "LMT", "STP", "STP_LMT", etc.
    double price;           // Price in USD (if applicable)
    int quantity;           // Order size
    std::string tif;        // Time in force (e.g., "DAY", "GTC")
    
    // IBKR-specific fields
    std::string exchange;   // e.g., "SMART", "NASDAQ", "NYSE"
    std::string currency;   // e.g., "USD"
    bool outsideRth;        // Allow outside regular trading hours
    
    // Order status tracking
    std::string orderId;
    std::string status;     // "Submitted", "Filled", "Cancelled", etc.
    double filled;          // Filled quantity
    double avgFillPrice;    // Average fill price
    
    Order() : conid(0), price(0), quantity(0), outsideRth(false),
             filled(0), avgFillPrice(0) {}
};

// Callbacks for real-time updates
using MarketDataCallback = std::function<void(const MarketData&)>;
using OrderUpdateCallback = std::function<void(const Order&)>;
using PositionUpdateCallback = std::function<void(const Position&)>;
using AccountUpdateCallback = std::function<void(const json&)>;

class RealIBKRInterface : public IBKRInterface {
public:
    RealIBKRInterface(const IBKRConfig& config);
    
    // Session management
    bool initializeSession();
    bool isConnected() const { return connected_; }
    
    // Market data operations
    bool subscribeMarketData(const std::string& symbol, 
                           const std::vector<std::string>& fields = {"TRADES"});
    bool unsubscribeMarketData(const std::string& symbol);
    
    // Order lifecycle methods
    json submitOrder(const Order& order);
    json modifyOrder(const std::string& orderId, const Order& updatedOrder);
    json cancelOrder(const std::string& orderId);
    json getOrderStatus(const std::string& orderId);
    
    // Account and position methods
    json getAccountSummary();
    json getPositions();
    json getOpenOrders();
    
    // Callback registration
    void setMarketDataCallback(MarketDataCallback callback) { marketDataCb_ = callback; }
    void setOrderCallback(OrderUpdateCallback callback) { orderCb_ = callback; }
    void setPositionCallback(PositionUpdateCallback callback) { positionCb_ = callback; }
    void setAccountCallback(AccountUpdateCallback callback) { accountCb_ = callback; }
    
protected:
    // HTTP request handling
    virtual json performHttpRequest(const std::string& method,
                                  const std::string& endpoint,
                                  const json& body = json::object());
                                  
    // Rate limiting
    bool checkRateLimit(const std::string& endpoint);
    void updateRateLimit(const std::string& endpoint);
    
    // Websocket handling for real-time data
    void startWebSocket();
    void stopWebSocket();
    void processWebSocketMessage(const std::string& message);
    
private:
    bool connected_;
    std::string sessionId_;
    IBKRConfig config_;
    
    // Callbacks
    MarketDataCallback marketDataCb_;
    OrderUpdateCallback orderCb_;
    PositionUpdateCallback positionCb_;
    AccountUpdateCallback accountCb_;
    
    // Rate limiting
    struct RateLimit {
        int maxRequests;
        int currentRequests;
        std::chrono::steady_clock::time_point resetTime;
    };
    std::unordered_map<std::string, RateLimit> rateLimits_;
    
    // Subscription tracking
    std::unordered_map<std::string, int> marketDataSubscriptions_;
    std::mutex subscriptionMutex_;
};

#endif // REAL_IBKR_INTERFACE_HPP