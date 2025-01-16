#include "api_client.hpp"
#include <curl/curl.h>
#include <stdexcept>
#include <iostream>

// Constructor
ApiClient::ApiClient(const std::string& base_url, const std::string& api_key)
    : base_url(base_url), api_key(api_key), headers(nullptr) {
    curl_global_init(CURL_GLOBAL_DEFAULT); // Initialize CURL globally
}

// Destructor
ApiClient::~ApiClient() {
    if (headers) {
        curl_slist_free_all(headers); // Free the header list
    }
    curl_global_cleanup(); // Clean up CURL globally
}

// Static helper function for handling HTTP responses
size_t ApiClient::writeCallback(void* contents, size_t size, size_t nmemb, std::string* userData) {
    userData->append(static_cast<char*>(contents), size * nmemb); // Append response data
    return size * nmemb;
}

// Add a custom header
void ApiClient::addHeader(const std::string& header) {
    headers = curl_slist_append(headers, header.c_str()); // Append header to the list
}

// Clear all custom headers
void ApiClient::clearHeaders() {
    if (headers) {
        curl_slist_free_all(headers); // Free the header list
        headers = nullptr;
    }
}

// Perform an HTTP request
std::string ApiClient::performRequest(const std::string& method, const std::string& endpoint, const std::string& payload) {
    CURL* curl = curl_easy_init(); // Initialize CURL
    if (!curl) {
        throw std::runtime_error("Failed to initialize CURL");
    }

    std::string response;
    std::string url = base_url + endpoint;

    try {
        // Set CURL options
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ApiClient::writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Add default headers
        addHeader("X-API-KEY: " + api_key); // Ensure the API key is always sent
        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers); // Attach headers to the request
        }

        // Handle different HTTP methods
        if (method == "POST" || method == "PUT" || method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        }

        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else if (method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        } else if (method == "DELETE") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        }

        // Perform the request
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            throw std::runtime_error("CURL error: " + std::string(curl_easy_strerror(res)));
        }

    } catch (const std::exception& e) {
        curl_easy_cleanup(curl); // Ensure cleanup on error
        throw; // Rethrow the exception
    }

    curl_easy_cleanup(curl); // Cleanup after successful request
    return response;
}

// Perform an HTTP GET request
std::string ApiClient::httpGet(const std::string& endpoint) {
    return performRequest("GET", endpoint, "");
}

// Perform an HTTP POST request
std::string ApiClient::httpPost(const std::string& endpoint, const std::string& payload) {
    return performRequest("POST", endpoint, payload);
}

// Perform an HTTP PUT request
std::string ApiClient::httpPut(const std::string& endpoint, const std::string& payload) {
    return performRequest("PUT", endpoint, payload);
}

// Perform an HTTP DELETE request
std::string ApiClient::httpDelete(const std::string& endpoint, const std::string& payload) {
    return performRequest("DELETE", endpoint, payload);
}
