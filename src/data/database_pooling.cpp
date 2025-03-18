#include "trade_ngin/data/database_pooling.hpp"

namespace trade_ngin {

Result<void> DatabasePool::initialize(const std::string& connection_string, size_t pool_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Initialize logger
    LoggerConfig logger_config;
    logger_config.min_level = LogLevel::INFO;
    logger_config.destination = LogDestination::BOTH;
    logger_config.log_directory = "logs";
    logger_config.filename_prefix = "database_pool";
    Logger::instance().initialize(logger_config);

    if (initialized_) {
        WARN("Database pool already initialized");
        return Result<void>();
    }

    // Create connections
    for (size_t i = 0; i < pool_size; ++i) {
        auto db = std::make_shared<PostgresDatabase>(connection_string);
        auto result = db->connect();
        if (result.is_ok()) {
            available_connections_.push_back(db);
        } else {
            ERROR("Failed to initialize connection in pool: " + 
                std::string(result.error()->what()));
        }
    }

    initialized_ = true;
    INFO("Database pool initialized with " + std::to_string(pool_size) + " connections");

    return Result<void>();
}

std::shared_ptr<PostgresDatabase> DatabasePool::create_new_connection() {
    // Only call this with mutex already locked
    auto db = std::make_shared<PostgresDatabase>(default_connection_string_);
    auto result = db->connect();
    if (result.is_ok()) {
        total_connections_++;
        INFO("Created new database connection. Total connections: " + 
            std::to_string(total_connections_));
        return db;
    } else {
        ERROR("Failed to create new connection: " + result.error()->to_string());
        return nullptr;
    }
}

DatabasePool::ConnectionGuard DatabasePool::acquire_connection(int max_retries, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
        
    auto start_time = std::chrono::steady_clock::now();
    int attempts = 0;
    
    while (available_connections_.empty() && attempts < max_retries) {
        // Wait for a connection to become available or timeout
        auto wait_result = cv_.wait_for(lock, timeout);
        
        if (wait_result == std::cv_status::timeout) {
            attempts++;
            WARN("Timeout waiting for database connection (attempt " + 
                    std::to_string(attempts) + "/" + std::to_string(max_retries) + ")");
            
            // Check if we need to create a new emergency connection
            if (attempts == max_retries && total_connections() < max_pool_size_) {
                INFO("Creating emergency connection to expand pool");
                auto db = create_new_connection();
                if (db) {
                    return ConnectionGuard(db, this);
                }
            }
        }
        
        // Check for timeout of entire operation
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed > std::chrono::seconds(30)) {
            ERROR("Connection acquisition timed out after 30 seconds");
            return ConnectionGuard(nullptr, this); // Return null connection
        }
    }
    
    if (available_connections_.empty()) {
        ERROR("No database connections available after retries");
        return ConnectionGuard(nullptr, this); // Return null connection
    }
    
    // Get a connection from the pool
    auto connection = available_connections_.front();
    available_connections_.pop_front();
    
    // Ensure the connection is still valid
    if (!connection->is_connected()) {
        INFO("Reconnecting stale database connection");
        auto reconnect_result = connection->connect();
        if (reconnect_result.is_error()) {
            ERROR("Failed to reconnect database: " + reconnect_result.error()->to_string());
            connection = create_new_connection();
        }
    }
    
    return ConnectionGuard(connection, this);
}

Result<void> DatabasePool::return_connection(std::shared_ptr<PostgresDatabase> connection) {
    if (!connection) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Null connection returned to pool",
            "DatabasePool"
        );
    };
        
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Verify connection still works before returning to pool
    if (!connection->is_connected()) {
        INFO("Reconnecting failed connection before returning to pool");
        auto result = connection->connect();
        if (result.is_error()) {
            ERROR("Failed to reconnect returned connection: " + result.error()->to_string());
            // Don't return a bad connection to the pool
            return Result<void>();
        }
    }
    
    available_connections_.push_back(connection);
    
    // Notify waiting threads
    cv_.notify_one();

    return Result<void>();
}

} // namespace trade_ngin