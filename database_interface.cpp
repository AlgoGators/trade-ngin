#include "database_interface.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

// Initialize static members
std::vector<std::unique_ptr<pqxx::connection>> DatabaseInterface::connection_pool_;
std::mutex DatabaseInterface::pool_mutex_;

DatabaseInterface::DatabaseInterface() {
    try {
        // Load environment variables
        EnvLoader::load(".env");
        
        // Initialize connection
        auto conn_str = getConnectionString();
        conn_ = std::make_unique<pqxx::connection>(conn_str);
        
        if (!validateConnection()) {
            throw std::runtime_error("Failed to validate database connection");
        }
        
        std::cout << "Database connection established successfully.\n";
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Database connection error: " + std::string(e.what()));
    }
}

bool DatabaseInterface::validateConnection() {
    try {
        // Try a simple query
        pqxx::work txn(*conn_);
        txn.exec1("SELECT 1");
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Connection validation failed: " << e.what() << std::endl;
        return false;
    }
}

std::string DatabaseInterface::getConnectionString() {
    const char* host = std::getenv("DB_HOST");
    const char* port = std::getenv("DB_PORT");
    const char* user = std::getenv("DB_USER");
    const char* password = std::getenv("DB_PASSWORD");
    const char* dbname = std::getenv("DB_NAME");

    if (!host || !port || !user || !password || !dbname) {
        throw std::runtime_error("Missing required environment variables");
    }

    return "host=" + std::string(host) +
           " port=" + std::string(port) +
           " user=" + std::string(user) +
           " password=" + std::string(password) +
           " dbname=" + std::string(dbname) +
           " connect_timeout=" + std::to_string(TIMEOUT_SECONDS);
}

std::shared_ptr<pqxx::connection> DatabaseInterface::getConnection() {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    
    // Try to get an existing connection
    for (auto& conn : connection_pool_) {
        if (conn && conn->is_open()) {
            auto shared_conn = std::shared_ptr<pqxx::connection>(
                conn.release(),
                [this](pqxx::connection* c) { 
                    this->releaseConnection(std::shared_ptr<pqxx::connection>(c));
                }
            );
            return shared_conn;
        }
    }
    
    // Create new connection if pool is empty
    auto conn_str = getConnectionString();
    auto new_conn = std::make_shared<pqxx::connection>(conn_str);
    return new_conn;
}

void DatabaseInterface::releaseConnection(std::shared_ptr<pqxx::connection> conn) {
    std::lock_guard<std::mutex> lock(pool_mutex_);
    if (connection_pool_.size() < MAX_RETRIES) {
        connection_pool_.push_back(std::unique_ptr<pqxx::connection>(conn.get()));
    }
} 