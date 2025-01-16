#pragma once
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include "env_loader.hpp"

class DatabaseInterface {
public:
    DatabaseInterface();
    ~DatabaseInterface() = default;
    
    // Prevent copying
    DatabaseInterface(const DatabaseInterface&) = delete;
    DatabaseInterface& operator=(const DatabaseInterface&) = delete;

    // Database operations
    std::shared_ptr<arrow::Table> getOHLCVArrowTable(
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& symbols
    );

    std::shared_ptr<arrow::Table> getSymbolsAsArrowTable();
    std::string getEarliestDate();
    std::string getLatestDate();
    std::shared_ptr<arrow::Table> getLatestDataAsArrowTable(const std::string& symbol);

private:
    std::unique_ptr<pqxx::connection> conn_;
    static constexpr int MAX_RETRIES = 3;
    static constexpr int TIMEOUT_SECONDS = 30;
    
    // Connection pooling
    static std::vector<std::unique_ptr<pqxx::connection>> connection_pool_;
    static std::mutex pool_mutex_;
    
    bool validateConnection();
    std::string getConnectionString();
    std::shared_ptr<pqxx::connection> getConnection();
    void releaseConnection(std::shared_ptr<pqxx::connection> conn);
}; 