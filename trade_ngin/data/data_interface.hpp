#ifndef DATA_INTERFACE_HPP
#define DATA_INTERFACE_HPP

#include <string>
#include <vector>
#include <memory>
#include <arrow/api.h>
#include <nlohmann/json.hpp>
#include "database_client.hpp"

class DataInterface {
public:
    explicit DataInterface(std::shared_ptr<DatabaseClient> client);

    // Data operations
    std::shared_ptr<arrow::Table> getOHLCV(const std::string& symbol, const std::string& start_date, const std::string& end_date);
    std::shared_ptr<arrow::Table> getSymbols();
    bool insertData(const std::string& symbol, const std::vector<std::map<std::string, std::string>>& data);
    bool updateData(const std::string& symbol, const std::string& date, const std::map<std::string, std::string>& updates);
    bool deleteData(const std::string& symbol, const std::string& start_date, const std::string& end_date);

private:
    std::shared_ptr<DatabaseClient> client;
};

#endif // DATA_INTERFACE_HPP
