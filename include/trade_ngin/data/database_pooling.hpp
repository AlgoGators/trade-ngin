#pragma once

#include <mutex>
#include <vector>
#include <deque>
#include <condition_variable>
#include <memory>
#include "trade_ngin/data/postgres_database.hpp"

namespace trade_ngin {
namespace utils {

// Retry function with exponential backoff
template<typename Func>
auto retry_with_backoff(Func func, int max_retries = 3) -> decltype(func()) {
    using ReturnType = decltype(func());
    
    int attempt = 0;
    std::chrono::milliseconds delay(100);  // Start with 100ms delay
    
    while (attempt < max_retries) {
        auto result = func();
        
        // Check if success or non-retryable error
        if (!result.is_error() || 
            (result.is_error() && 
            result.error()->code() != ErrorCode::DATABASE_ERROR)) {
            return result;
        }
        
        // Log retry attempt
        WARN("Database operation failed, retrying (attempt " + std::to_string(attempt + 1) + 
             " of " + std::to_string(max_retries) + "): " + result.error()->what());
        
        // Wait before retry
        std::this_thread::sleep_for(delay);
        
        // Exponential backoff with jitter
        delay *= 2;
        delay += std::chrono::milliseconds(rand() % 100);
        
        attempt++;
    }
    
    // All retries failed, execute one last time and return the result
    return func();
}
} // namespace utils

/**
 * @brief Database connection pool for managing multiple connections
 */
// DatabasePool class definition
class DatabasePool {
public:
    /**
     * @brief Get the singleton instance of the database pool
     * @return Reference to the database pool instance
     */
    static DatabasePool& instance() {
        static DatabasePool pool;
        return pool;
    }

    /**
     * @brief Initialize the database pool with a set of connections
     * @param connection_string Connection string for the database
     * @param pool_size Number of connections to create
     * @return Result indicating success or failure
     */
    Result<void> initialize(const std::string& connection_string, size_t pool_size = 5);

    /**
     * @brief Connection guard class for managing connection lifecycle
     */
    // ConnectionGuard class definition
    class ConnectionGuard {
    public:
        /**
         * @brief Constructor
         * @param connection Shared pointer to the database connection
         * @param pool Pointer to the database pool
         */
        ConnectionGuard(std::shared_ptr<PostgresDatabase> connection, DatabasePool* pool)
        : connection_(connection), pool_(pool) {}

        /**
         * @brief Destructor
         */
        ~ConnectionGuard() {
            if (connection_ && pool_) {
                pool_->return_connection(connection_);
            }
        }

        /**
         * @brief Get the shared pointer to the database connection
         * @return Shared pointer to the database connection
         */
        std::shared_ptr<PostgresDatabase> get() const {
            return connection_;
        }

        // Disable copying
        ConnectionGuard(const ConnectionGuard&) = delete;
        ConnectionGuard& operator=(const ConnectionGuard&) = delete;

        // Allow moving
        ConnectionGuard(ConnectionGuard&& other) noexcept 
            : connection_(std::move(other.connection_)), pool_(other.pool_) {
            other.pool_ = nullptr;
        }

        ConnectionGuard& operator=(ConnectionGuard&& other) noexcept {
            if (this != &other) {
                // Return our current connection if we have one
                if (connection_ && pool_) {
                    pool_->return_connection(connection_);
                }
                
                // Take ownership of other's connection
                connection_ = std::move(other.connection_);
                pool_ = other.pool_;
                other.pool_ = nullptr;
            }
            return *this;
        }

    private:
        std::shared_ptr<PostgresDatabase> connection_;
        DatabasePool* pool_;
    };

    /**
     * @brief Acquire a connection from the pool
     * @param max_retries Maximum number of retries to acquire a connection
     * @param timeout Timeout for acquiring a connection
     * @return ConnectionGuard object containing the acquired connection
     */
    ConnectionGuard acquire_connection(int max_retries= 3, 
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

    /**
     * @brief Return a connection to the pool
     * @param connection Shared pointer to the database connection
     * @return Result indicating success or failure
     */
    Result<void> return_connection(std::shared_ptr<PostgresDatabase> connection);

    size_t available_connections_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_connections_.size();
    }

    size_t total_connections() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return available_connections_.size() + (total_connections_ - available_connections_.size());
    }
    
private:
    DatabasePool() : initialized_(false), total_connections_(0), max_pool_size_(20) {}
    ~DatabasePool() = default;

    bool initialized_;
    size_t total_connections_;
    size_t max_pool_size_;
    std::string default_connection_string_;
    std::deque<std::shared_ptr<PostgresDatabase>> available_connections_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::shared_ptr<PostgresDatabase> create_new_connection();
};

} // namespace trade_ngin