#ifndef HTTP_REQUEST_HPP
#define HTTP_REQUEST_HPP

#include "Logger.hpp"
#include "error_handler.hpp"
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <string>
#include <chrono>
#include <random>
#include <thread>

using json = nlohmann::json;

class HttpRequest {
public:
    // Callback for libcurl to write response data
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    // Main request method with retry logic
    static json performRequest(const std::string& method,
                             const std::string& url,
                             const std::string& authToken,
                             const json& body = json::object(),
                             int maxRetries = 3,
                             std::chrono::milliseconds initialDelay = std::chrono::milliseconds(100)) {
        CURL* curl = nullptr;
        CURLcode res;
        std::string readBuffer;
        long httpCode = 0;
        int retryCount = 0;
        std::chrono::milliseconds delay = initialDelay;
        
        while (true) {
            try {
                // Initialize CURL
                curl = curl_easy_init();
                if (!curl) {
                    throw std::runtime_error("Failed to initialize CURL");
                }
                
                struct curl_slist* headers = nullptr;
                headers = curl_slist_append(headers, "Content-Type: application/json");
                if (!authToken.empty()) {
                    std::string authHeader = "Authorization: Bearer " + authToken;
                    headers = curl_slist_append(headers, authHeader.c_str());
                }
                
                // Set common options
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
                
                // Set timeouts
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
                curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
                
                // Set method-specific options
                if (method == "POST") {
                    curl_easy_setopt(curl, CURLOPT_POST, 1L);
                    if (!body.empty()) {
                        std::string jsonStr = body.dump();
                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
                    }
                } else if (method == "DELETE") {
                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                }
                
                // Perform request
                res = curl_easy_perform(curl);
                
                // Get HTTP response code
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
                
                // Cleanup
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                
                // Handle errors
                if (res != CURLE_OK) {
                    throw std::runtime_error(std::string("CURL error: ") + 
                                          curl_easy_strerror(res));
                }
                
                // Check HTTP status code
                if (httpCode >= 400) {
                    throw std::runtime_error("HTTP error " + std::to_string(httpCode) + 
                                          ": " + readBuffer);
                }
                
                // Parse response
                json responseJson;
                try {
                    responseJson = json::parse(readBuffer);
                } catch (const json::parse_error& e) {
                    throw std::runtime_error("Failed to parse response: " + 
                                          std::string(e.what()));
                }
                
                // Log success
                Logger::getInstance().debug("HTTP {} {} succeeded with status {}", 
                                         method, url, httpCode);
                
                return responseJson;
                
            } catch (const std::exception& e) {
                if (curl) curl_easy_cleanup(curl);
                
                // Check if we should retry
                if (retryCount >= maxRetries) {
                    ErrorHandler::getInstance().recordError(
                        e.what(),
                        ErrorHandler::Severity::ERROR,
                        ErrorHandler::Category::NETWORK,
                        "Max retries exceeded for " + method + " " + url
                    );
                    throw;
                }
                
                // Log retry attempt
                Logger::getInstance().warning(
                    "Request failed (attempt {}/{}): {}. Retrying in {}ms...",
                    retryCount + 1, maxRetries, e.what(), delay.count()
                );
                
                // Wait before retry with exponential backoff
                std::this_thread::sleep_for(delay);
                delay *= 2;
                retryCount++;
            }
        }
    }
    
    // Convenience methods for common HTTP verbs
    static json get(const std::string& url, const std::string& authToken) {
        return performRequest("GET", url, authToken);
    }
    
    static json post(const std::string& url, 
                    const std::string& authToken,
                    const json& body = json::object()) {
        return performRequest("POST", url, authToken, body);
    }
    
    static json put(const std::string& url,
                   const std::string& authToken,
                   const json& body = json::object()) {
        return performRequest("PUT", url, authToken, body);
    }
    
    static json del(const std::string& url, const std::string& authToken) {
        return performRequest("DELETE", url, authToken);
    }
};

#endif // HTTP_REQUEST_HPP 