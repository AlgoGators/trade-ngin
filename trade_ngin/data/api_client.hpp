#ifndef API_CLIENT_HPP
#define API_CLIENT_HPP

#include <string>
#include <curl/curl.h>
#include <vector>
#include <stdexcept>

/**
 * @class ApiClient
 * @brief A lightweight HTTP client for interacting with REST APIs.
 *
 * This class provides methods for performing HTTP requests (GET, POST, PUT, DELETE)
 * with support for custom headers, payloads, and response handling.
 */
class ApiClient {
public:
    /**
     * @brief Constructs an ApiClient instance.
     * @param base_url The base URL of the API (e.g., "http://127.0.0.1:8000").
     * @param api_key The API key for authentication.
     */
    ApiClient(const std::string& base_url, const std::string& api_key);

    /**
     * @brief Destructor to clean up CURL resources.
     */
    ~ApiClient();

    /**
     * @brief Performs an HTTP GET request.
     * @param endpoint The API endpoint to send the request to (relative to base_url).
     * @return The response body as a string.
     * @throws std::runtime_error if the request fails.
     */
    std::string httpGet(const std::string& endpoint);

    /**
     * @brief Performs an HTTP POST request.
     * @param endpoint The API endpoint to send the request to (relative to base_url).
     * @param payload The request payload as a string (e.g., JSON or binary data).
     * @return The response body as a string.
     * @throws std::runtime_error if the request fails.
     */
    std::string httpPost(const std::string& endpoint, const std::string& payload);

    /**
     * @brief Performs an HTTP PUT request.
     * @param endpoint The API endpoint to send the request to (relative to base_url).
     * @param payload The request payload as a string.
     * @return The response body as a string.
     * @throws std::runtime_error if the request fails.
     */
    std::string httpPut(const std::string& endpoint, const std::string& payload);

    /**
     * @brief Performs an HTTP DELETE request.
     * @param endpoint The API endpoint to send the request to (relative to base_url).
     * @param payload The request payload as a string (optional).
     * @return The response body as a string.
     * @throws std::runtime_error if the request fails.
     */
    std::string httpDelete(const std::string& endpoint, const std::string& payload);

    /**
     * @brief Adds a custom header to be included in the next HTTP request.
     * @param header The header string (e.g., "Content-Type: application/json").
     */
    void addHeader(const std::string& header);

    /**
     * @brief Clears all custom headers.
     */
    void clearHeaders();

private:
    /**
     * @brief Helper function for performing HTTP requests.
     * @param method The HTTP method (e.g., "GET", "POST", "PUT", "DELETE").
     * @param endpoint The API endpoint to send the request to.
     * @param payload The request payload as a string.
     * @return The response body as a string.
     * @throws std::runtime_error if the request fails.
     */
    std::string performRequest(const std::string& method, const std::string& endpoint, const std::string& payload);

    /**
     * @brief Callback function for handling CURL write operations.
     * @param contents The data received from the server.
     * @param size The size of each data block.
     * @param nmemb The number of data blocks.
     * @param userData A pointer to the user-defined string where the response will be stored.
     * @return The total number of bytes written.
     */
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* userData);

    std::string base_url;                ///< The base URL for the API.
    std::string api_key;                 ///< The API key for authentication.
    struct curl_slist* headers;          ///< The list of custom headers.
};

#endif // API_CLIENT_HPP
