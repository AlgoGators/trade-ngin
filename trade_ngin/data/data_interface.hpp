#ifndef DATA_INTERFACE_HPP
#define DATA_INTERFACE_HPP

#include <string>
#include <vector>
#include <memory>
#include <arrow/api.h>
#include <nlohmann/json.hpp>
#include "api_client.hpp"

class DataInterface {
public:
    explicit DataInterface(std::shared_ptr<ApiClient> client);

    // Data operations
    std::shared_ptr<arrow::Table> getOHLCV(const std::string& start_date, const std::string& end_date, const std::vector<std::string>& symbols = {});
    std::shared_ptr<arrow::Table> getSymbols();
    std::string getEarliestDate();
    std::string getLatestDate();
    bool insertData(const std::string& schema, const std::string& table, const std::string& format, const nlohmann::json& payload_json, const std::shared_ptr<arrow::Table>& payload_arrow);
    bool updateData(const std::string& table, const nlohmann::json& filters, const nlohmann::json& updates);
    bool deleteData(const std::string& table, const nlohmann::json& filters);

private:
    std::shared_ptr<ApiClient> client;
};

#endif // DATA_INTERFACE_HPP
