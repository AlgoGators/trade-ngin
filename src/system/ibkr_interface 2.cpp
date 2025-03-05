#include "ibkr_interface.hpp"
#include <sstream>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <spdlog/sinks/rotating_file_sink.h>

IBKRInterface::IBKRInterface(const std::string& api_endpoint, const std::string& account_id) 
    : base_url(api_endpoint), account_id(account_id), connected(false) {
    
    // Initialize logging
    try {
        logger = spdlog::rotating_logger_mt("ibkr_interface", "logs/ibkr_interface.log", 
                                          1024 * 1024 * 5, 3);
        logger->set_level(spdlog::level::debug);
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v");
    } catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
        throw;
    }

    logger->info("Initializing IBKR interface with endpoint: {}", api_endpoint);

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_ALL);
    setupCurlHandle();

    // Initialize rate limits
    rate_limits["/iserver/marketdata/snapshot"] = {10, 0, std::chrono::steady_clock::now()};
    rate_limits["/iserver/account/orders"] = {1, 0, std::chrono::steady_clock::now()};
    // Add other endpoints as needed
}

IBKRInterface::~IBKRInterface() {
    if (curl) {
        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    logger->info("IBKR interface destroyed");
}

void IBKRInterface::setupCurlHandle() {
    curl = curl_easy_init();
    if (!curl) {
        logger->error("Failed to initialize CURL");
        throw std::runtime_error("CURL initialization failed");
    }

    // Set common CURL options
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
}

size_t IBKRInterface::WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

bool IBKRInterface::authenticate() {
    logger->debug("Attempting authentication");
    
    try {
        json response = makeRequest("/iserver/auth/status", "GET");
        
        if (response.contains("authenticated") && response["authenticated"].get<bool>()) {
            connected = true;
            logger->info("Authentication successful");
            return true;
        }
        
        // If not authenticated, try to authenticate
        response = makeRequest("/iserver/authenticate", "POST");
        
        if (response.contains("authenticated") && response["authenticated"].get<bool>()) {
            connected = true;
            logger->info("Authentication successful after explicit request");
            return true;
        }
        
        logger->error("Authentication failed");
        return false;
    } catch (const std::exception& e) {
        logger->error("Authentication error: {}", e.what());
        return false;
    }
}

json IBKRInterface::makeRequest(const std::string& endpoint, const std::string& method, const json& data) {
    if (!checkRateLimit(endpoint)) {
        logger->warn("Rate limit exceeded for endpoint: {}", endpoint);
        throw std::runtime_error("Rate limit exceeded");
    }

    std::string url = base_url + endpoint;
    std::string response_string;
    
    logger->debug("Making {} request to: {}", method, url);

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());

    // Add headers
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    if (!data.empty()) {
        std::string json_str = data.dump();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
        logger->debug("Request payload: {}", json_str);
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        logger->error("CURL request failed: {}", curl_easy_strerror(res));
        throw std::runtime_error(std::string("CURL request failed: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    logger->debug("Response code: {}", http_code);
    logger->debug("Response body: {}", response_string);

    updateRateLimit(endpoint);

    if (http_code >= 400) {
        logger->error("HTTP error {}: {}", http_code, response_string);
        throw std::runtime_error("HTTP error " + std::to_string(http_code));
    }

    try {
        return json::parse(response_string);
    } catch (const json::parse_error& e) {
        logger->error("JSON parse error: {}", e.what());
        throw;
    }
}

bool IBKRInterface::checkRateLimit(const std::string& endpoint) {
    auto now = std::chrono::steady_clock::now();
    auto& limit = rate_limits[endpoint];

    if (now >= limit.reset_time) {
        limit.current_requests = 0;
        limit.reset_time = now + std::chrono::seconds(1);
    }

    return limit.current_requests < limit.max_requests;
}

void IBKRInterface::updateRateLimit(const std::string& endpoint) {
    auto& limit = rate_limits[endpoint];
    limit.current_requests++;
}

json IBKRInterface::getMarketData(const std::string& symbol, const std::vector<std::string>& fields) {
    if (!isConnected() && !authenticate()) {
        throw std::runtime_error("Not connected to IBKR");
    }

    std::stringstream ss;
    ss << "/iserver/marketdata/snapshot?conids=" << symbol;
    if (!fields.empty()) {
        ss << "&fields=";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i > 0) ss << ",";
            ss << fields[i];
        }
    }

    logger->info("Requesting market data for symbol: {}", symbol);
    return makeRequest(ss.str(), "GET");
}

json IBKRInterface::placeOrder(const std::string& symbol, double quantity, double price, bool is_buy) {
    if (!isConnected() && !authenticate()) {
        throw std::runtime_error("Not connected to IBKR");
    }

    json order_data = {
        {"acctId", account_id},
        {"conid", symbol},
        {"orderType", "LMT"},
        {"price", price},
        {"side", is_buy ? "BUY" : "SELL"},
        {"quantity", quantity},
        {"tif", "DAY"}
    };

    logger->info("Placing order: {} {} {} @ {}", 
                 is_buy ? "BUY" : "SELL", quantity, symbol, price);
    
    return makeRequest("/iserver/account/" + account_id + "/orders", "POST", order_data);
}

bool IBKRInterface::isConnected() const {
    return connected;
}

json IBKRInterface::getAccountSummary() {
    if (!isConnected() && !authenticate()) {
        throw std::runtime_error("Not connected to IBKR");
    }

    logger->info("Requesting account summary");
    return makeRequest("/portfolio/" + account_id + "/summary", "GET");
}

json IBKRInterface::getPositions() {
    if (!isConnected() && !authenticate()) {
        throw std::runtime_error("Not connected to IBKR");
    }

    logger->info("Requesting positions");
    return makeRequest("/portfolio/" + account_id + "/positions", "GET");
}

json IBKRInterface::getOpenOrders() {
    if (!isConnected() && !authenticate()) {
        throw std::runtime_error("Not connected to IBKR");
    }

    logger->info("Requesting open orders");
    return makeRequest("/iserver/account/orders", "GET");
} 