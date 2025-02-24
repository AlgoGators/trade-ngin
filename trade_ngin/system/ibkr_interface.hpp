#pragma once

// Include TWS API headers first
#include <EClient.h>
#include <EClientSocket.h>
#include <EReader.h>
#include <EReaderSignal.h>
#include <EWrapper.h>
#include <Contract.h>
#include <Order.h>
#include <TagValue.h>

// Standard library headers
#include <string>
#include <memory>
#include <vector>
#include <map>
#include <functional>

// Threading support headers
#include <thread>
#include <chrono>

// Local includes
#include "ibkr/market_data_handler.hpp"
#include "ibkr/account_handler.hpp"
#include "ibkr_wrapper.hpp"

// Forward declarations
class IBKRWrapper;

namespace ibkr {

// Type definitions for callbacks
using MarketDataCallback = std::function<void(const ::Contract&, double, const std::string&)>;
using AccountUpdateCallback = std::function<void(const std::string&, const std::string&, const std::string&, const std::string&)>;
using PositionUpdateCallback = std::function<void(const std::string&, const ::Contract&, double, double)>;
using OrderUpdateCallback = std::function<void(const ::Contract&, const ::Order&, const std::string&)>;

// Configuration structures
struct APISettings {
    bool readOnly = false;
    std::string encoding = "UTF-8";
    bool downloadOpenOrders = false;
    bool includeFX = false;
    bool prepareDailyPnL = false;
    bool exposeSchedule = false;
    bool useAccountGroups = false;
};

struct TradingSettings {
    int maxPositionSize = 100;
    double maxOrderValue = 10000.0;
    std::string defaultOrderType = "MARKET";
    bool simulationMode = true;
};

struct PaperTradingLimits {
    double maxLeverage = 4.0;
    double maxPositionValue = 100000.0;
};

struct RiskSettings {
    double maxDailyLoss = 1000.0;
    double maxPositionLoss = 500.0;
    PaperTradingLimits paperTradingLimits;
};

struct IBKRConfig {
    std::string host;
    int port;
    int clientId;
    bool useLogger;
    std::string accountId;
    std::string connectionOptions;
    APISettings api_settings;
    TradingSettings trading;
    RiskSettings risk;
};

class IBKRInterface {
public:
    explicit IBKRInterface(const std::string& config_path);
    ~IBKRInterface();

    // Connection management
    bool connect();
    void disconnect();
    bool isConnected() const;
    void processMessages();

    // Market data operations
    bool requestMarketData(const ::Contract& contract, const std::vector<std::string>& genericTicks = {});
    bool cancelMarketData(const ::Contract& contract);

    // Order operations
    std::string placeOrder(const ::Contract& contract, const ::Order& order);

    // Callback setters
    void setMarketDataCallback(MarketDataCallback callback);
    void setAccountCallback(AccountUpdateCallback callback);
    void setPositionCallback(PositionUpdateCallback callback);
    void setOrderCallback(OrderUpdateCallback callback);

private:
    void loadConfig(const std::string& config_path);
    bool checkServerVersion(int minVersion, const std::string& operation) const;

    // TWS API components
    std::unique_ptr<IBKRWrapper> wrapper_;
    std::unique_ptr<::EReaderSignal> signal_;
    std::unique_ptr<::EClientSocket> client_;
    std::unique_ptr<::EReader> reader_;

    // State
    bool connected_;
    int nextOrderId_;
    int serverVersion_;
    IBKRConfig config_;
    std::map<int, ::Contract> activeRequests_;
};

} // namespace ibkr