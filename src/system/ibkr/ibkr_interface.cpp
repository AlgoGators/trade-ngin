#include "ibkr_interface.hpp"
#include "ibkr_wrapper.hpp"
#include "ibkr/market_data_handler.hpp"
#include "ibkr/account_handler.hpp"

// Standard library headers first
#include <thread>
#include <chrono>
#include <string>
#include <memory>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <limits>

// Define RapidJSON configuration before including it
#define RAPIDJSON_HAS_STDSTRING 1
#define RAPIDJSON_HAS_CXX11_RVALUE_REFS 1
#define RAPIDJSON_NAMESPACE rapidjson
#define RAPIDJSON_NAMESPACE_BEGIN namespace rapidjson {
#define RAPIDJSON_NAMESPACE_END }

// Third-party headers
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <spdlog/spdlog.h>

namespace {

/**
 * @brief Helper function to read entire file content into a string
 * @param path Path to the file to read
 * @return String containing the file contents
 * @throws std::runtime_error if file cannot be opened
 */
std::string readFile(const std::string& path) {
    std::string content;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    try {
        // Get file size
        std::fseek(fp, 0, SEEK_END);
        long size = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);

        // Read file content
        content.resize(size);
        if (std::fread(&content[0], 1, size, fp) != static_cast<std::size_t>(size)) {
            throw std::runtime_error("Failed to read file: " + path);
        }
    } catch (...) {
        std::fclose(fp);
        throw;
    }

    std::fclose(fp);
    return content;
}

template<typename T>
T getValueOrDefault(const rapidjson::Value& obj, const char* key, const T& defaultValue) {
    if (!obj.HasMember(key)) return defaultValue;
    const auto& value = obj[key];
    
    if constexpr (std::is_same_v<T, std::string>) {
        return value.IsString() ? value.GetString() : defaultValue;
    } else if constexpr (std::is_same_v<T, int>) {
        return value.IsInt() ? value.GetInt() : defaultValue;
    } else if constexpr (std::is_same_v<T, bool>) {
        return value.IsBool() ? value.GetBool() : defaultValue;
    } else if constexpr (std::is_same_v<T, double>) {
        return value.IsNumber() ? value.GetDouble() : defaultValue;
    }
    return defaultValue;
}

} // anonymous namespace

namespace ibkr {

using namespace std::chrono_literals;
using namespace rapidjson;

IBKRInterface::IBKRInterface(const std::string& config_path) 
    : connected_(false), nextOrderId_(-1), serverVersion_(0) {
    spdlog::info("Initializing IBKRInterface with config path: {}", config_path);
    loadConfig(config_path);
    
    // Initialize TWS API components
    spdlog::debug("Creating TWS API components");
    wrapper_ = std::make_unique<IBKRWrapper>();
    signal_ = std::make_unique<::EReaderSignal>();
    client_ = std::make_unique<::EClientSocket>(wrapper_.get(), signal_.get());
    spdlog::info("IBKRInterface initialization complete");
}

IBKRInterface::~IBKRInterface() {
    if (isConnected()) {
        disconnect();
    }
}

void IBKRInterface::loadConfig(const std::string& config_path) {
    spdlog::info("Loading configuration from: {}", config_path);
    try {
        std::string config_str = readFile(config_path);
        spdlog::debug("Configuration file contents loaded, size: {} bytes", config_str.size());
        
        Document doc;
        ParseResult ok = doc.Parse(config_str.c_str());
        if (!ok) {
            std::string error = std::string("JSON parse error: ") + GetParseError_En(ok.Code());
            spdlog::error("Failed to parse config: {} at offset {}", error, ok.Offset());
            throw std::runtime_error(error);
        }
        
        spdlog::info("Successfully parsed JSON configuration");
        spdlog::debug("Parsing individual configuration values...");
        
        // Load basic settings
        config_.host = getValueOrDefault<std::string>(doc, "host", "127.0.0.1");
        spdlog::debug("Found host configuration: {}", config_.host);
        config_.port = getValueOrDefault<int>(doc, "port", 7497);  // Default to paper trading
        spdlog::debug("Found port configuration: {}", config_.port);
        config_.clientId = getValueOrDefault<int>(doc, "clientId", 0);
        spdlog::debug("Found clientId configuration: {}", config_.clientId);
        config_.useLogger = getValueOrDefault<bool>(doc, "useLogger", true);
        spdlog::debug("Found useLogger configuration: {}", config_.useLogger);
        config_.accountId = getValueOrDefault<std::string>(doc, "accountId", "");
        spdlog::debug("Found accountId configuration: {}", config_.accountId);
        config_.connectionOptions = getValueOrDefault<std::string>(doc, "connectionOptions", "");
        spdlog::debug("Found connectionOptions configuration: {}", config_.connectionOptions);
        
        // Load API settings
        if (doc.HasMember("api_settings")) {
            const auto& api = doc["api_settings"];
            config_.api_settings.readOnly = getValueOrDefault<bool>(api, "readOnly", false);
            spdlog::debug("Found readOnly configuration: {}", config_.api_settings.readOnly);
            config_.api_settings.encoding = getValueOrDefault<std::string>(api, "encoding", "UTF-8");
            spdlog::debug("Found encoding configuration: {}", config_.api_settings.encoding);
            config_.api_settings.downloadOpenOrders = getValueOrDefault<bool>(api, "downloadOpenOrders", false);
            spdlog::debug("Found downloadOpenOrders configuration: {}", config_.api_settings.downloadOpenOrders);
            config_.api_settings.includeFX = getValueOrDefault<bool>(api, "includeFX", false);
            spdlog::debug("Found includeFX configuration: {}", config_.api_settings.includeFX);
            config_.api_settings.prepareDailyPnL = getValueOrDefault<bool>(api, "prepareDailyPnL", false);
            spdlog::debug("Found prepareDailyPnL configuration: {}", config_.api_settings.prepareDailyPnL);
            config_.api_settings.exposeSchedule = getValueOrDefault<bool>(api, "exposeSchedule", false);
            spdlog::debug("Found exposeSchedule configuration: {}", config_.api_settings.exposeSchedule);
            config_.api_settings.useAccountGroups = getValueOrDefault<bool>(api, "useAccountGroups", false);
            spdlog::debug("Found useAccountGroups configuration: {}", config_.api_settings.useAccountGroups);
        }
        
        // Load trading settings
        if (doc.HasMember("trading")) {
            const auto& trading = doc["trading"];
            config_.trading.maxPositionSize = getValueOrDefault<int>(trading, "maxPositionSize", 100);
            spdlog::debug("Found maxPositionSize configuration: {}", config_.trading.maxPositionSize);
            config_.trading.maxOrderValue = getValueOrDefault<double>(trading, "maxOrderValue", 10000.0);
            spdlog::debug("Found maxOrderValue configuration: {}", config_.trading.maxOrderValue);
            config_.trading.defaultOrderType = getValueOrDefault<std::string>(trading, "defaultOrderType", "MARKET");
            spdlog::debug("Found defaultOrderType configuration: {}", config_.trading.defaultOrderType);
            config_.trading.simulationMode = getValueOrDefault<bool>(trading, "simulationMode", true);
            spdlog::debug("Found simulationMode configuration: {}", config_.trading.simulationMode);
        }
        
        // Load risk settings
        if (doc.HasMember("risk")) {
            const auto& risk = doc["risk"];
            config_.risk.maxDailyLoss = getValueOrDefault<double>(risk, "maxDailyLoss", 1000.0);
            spdlog::debug("Found maxDailyLoss configuration: {}", config_.risk.maxDailyLoss);
            config_.risk.maxPositionLoss = getValueOrDefault<double>(risk, "maxPositionLoss", 500.0);
            spdlog::debug("Found maxPositionLoss configuration: {}", config_.risk.maxPositionLoss);
            
            if (risk.HasMember("paperTradingLimits")) {
                const auto& limits = risk["paperTradingLimits"];
                config_.risk.paperTradingLimits.maxLeverage = getValueOrDefault<double>(limits, "maxLeverage", 4.0);
                spdlog::debug("Found maxLeverage configuration: {}", config_.risk.paperTradingLimits.maxLeverage);
                config_.risk.paperTradingLimits.maxPositionValue = getValueOrDefault<double>(limits, "maxPositionValue", 100000.0);
                spdlog::debug("Found maxPositionValue configuration: {}", config_.risk.paperTradingLimits.maxPositionValue);
            }
        }
        
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config from {}: {}", config_path, e.what());
        throw;
    }
}

bool IBKRInterface::connect() {
    if (isConnected()) {
        spdlog::warn("Already connected to TWS");
        return true;
    }

    spdlog::info("Attempting to connect to TWS at {}:{}", config_.host, config_.port);
    
    if (!client_->connect(config_.host.c_str(), config_.port, config_.clientId, false)) {
        spdlog::error("Failed to connect to TWS");
        return false;
    }

    spdlog::debug("Connection established, waiting for nextValidId");
    
    // Wait for nextValidId to be received
    int attempts = 0;
    const int maxAttempts = 50;
    while (nextOrderId_ == -1 && attempts < maxAttempts) {
        std::this_thread::sleep_for(100ms);
        attempts++;
        spdlog::trace("Waiting for nextValidId, attempt {}/{}", attempts, maxAttempts);
    }

    if (nextOrderId_ == -1) {
        spdlog::error("Timed out waiting for nextValidId");
        disconnect();
        return false;
    }

    connected_ = true;
    serverVersion_ = client_->serverVersion();
    spdlog::info("Successfully connected to TWS. Server version: {}, Next order ID: {}", 
                 serverVersion_, nextOrderId_);
    return true;
}

void IBKRInterface::disconnect() {
    if (isConnected()) {
        client_->disconnect();
        connected_ = false;
        spdlog::info("Disconnected from TWS");
    }
}

bool IBKRInterface::isConnected() const {
    return connected_ && client_->isConnected();
}

void IBKRInterface::processMessages() {
    reader_->processMsgs();
}

bool IBKRInterface::checkServerVersion(int minVersion, const std::string& operation) const {
    if (serverVersion_ < minVersion) {
        spdlog::error("Server version {} does not support {}", serverVersion_, operation);
        return false;
    }
    return true;
}

bool IBKRInterface::requestMarketData(const ::Contract& contract, 
                                    const std::vector<std::string>& genericTicks) {
    if (!isConnected()) {
        spdlog::error("Cannot request market data - not connected to TWS");
        return false;
    }

    spdlog::info("Requesting market data for contract: {} {}", 
                 contract.symbol, contract.secType);
    spdlog::debug("Contract details - Exchange: {}, Currency: {}, Multiplier: {}", 
                  contract.exchange, contract.currency, contract.multiplier);

    // Log generic ticks being requested
    if (!genericTicks.empty()) {
        std::string tickTypes;
        for (const auto& tick : genericTicks) {
            tickTypes += tick + " ";
        }
        spdlog::debug("Requesting generic tick types: {}", tickTypes);
    }

    // Generate a unique request ID
    static int nextReqId = 1;
    int reqId = nextReqId++;

    // Store the contract for later reference
    activeRequests_[reqId] = contract;

    // Convert generic ticks to string
    std::string genericTickList;
    for (const auto& tick : genericTicks) {
        if (!genericTickList.empty()) {
            genericTickList += ",";
        }
        genericTickList += tick;
    }

    client_->reqMktData(reqId, contract, genericTickList, false, false, {});
    return true;
}

bool IBKRInterface::cancelMarketData(const ::Contract& contract) {
    if (!isConnected()) {
        spdlog::error("Cannot cancel market data - not connected to TWS");
        return false;
    }

    // Find the request ID for this contract
    for (const auto& [reqId, storedContract] : activeRequests_) {
        if (storedContract.conId == contract.conId) {
            client_->cancelMktData(reqId);
            activeRequests_.erase(reqId);
            return true;
        }
    }

    spdlog::warn("No active market data request found for contract");
    return false;
}

std::string IBKRInterface::placeOrder(const ::Contract& contract, const ::Order& order) {
    if (!isConnected()) {
        throw std::runtime_error("Not connected to TWS");
    }

    // Validate order against risk limits
    if (!config_.trading.simulationMode) {
        double orderValue = order.totalQuantity * contract.strike;
        if (orderValue > config_.trading.maxOrderValue) {
            throw std::runtime_error("Order value exceeds maximum allowed");
        }
    }

    // Generate order ID
    int orderId = nextOrderId_++;
    
    // Place the order
    client_->placeOrder(orderId, contract, order);
    
    return std::to_string(orderId);
}

void IBKRInterface::setMarketDataCallback(MarketDataCallback callback) {
    if (wrapper_) {
        wrapper_->setMarketDataCallback(callback);
    }
}

void IBKRInterface::setAccountCallback(AccountUpdateCallback callback) {
    if (wrapper_) {
        wrapper_->setAccountCallback(callback);
    }
}

void IBKRInterface::setPositionCallback(PositionUpdateCallback callback) {
    if (wrapper_) {
        wrapper_->setPositionCallback(callback);
    }
}

void IBKRInterface::setOrderCallback(OrderUpdateCallback callback) {
    if (wrapper_) {
        wrapper_->setOrderCallback(callback);
    }
}

} // namespace ibkr