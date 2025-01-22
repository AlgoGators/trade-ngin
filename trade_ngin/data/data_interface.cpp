#define NOMINMAX
#include "data_interface.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/ipc/writer.h>
#include <arrow/ipc/reader.h>
#include <stdexcept>
#include <iostream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Constructor
DataInterface::DataInterface(std::shared_ptr<DatabaseClient> client) : client(client) {}

// Retrieve OHLCV data
std::shared_ptr<arrow::Table> DataInterface::getOHLCV(const std::string& symbol, const std::string& start_date, const std::string& end_date) {
    std::stringstream query;
    query << "SELECT date, open, high, low, close, volume FROM market_data.ohlcv "
          << "WHERE symbol = '" << symbol << "' "
          << "AND date >= '" << start_date << "' "
          << "AND date <= '" << end_date << "' "
          << "ORDER BY date ASC";

    auto schema = arrow::schema({
        arrow::field("date", arrow::date32()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::int64())
    });

    return client->fetchDataAsArrowTable(query.str(), schema);
}

// Retrieve symbols
std::shared_ptr<arrow::Table> DataInterface::getSymbols() {
    std::string query = "SELECT DISTINCT symbol FROM market_data.ohlcv ORDER BY symbol ASC";
    
    auto schema = arrow::schema({
        arrow::field("symbol", arrow::utf8())
    });

    return client->fetchDataAsArrowTable(query, schema);
}

// Insert data using JSON or Apache Arrow
bool DataInterface::insertData(const std::string& symbol, const std::vector<std::map<std::string, std::string>>& data) {
    try {
        client->insertData("market_data", "ohlcv", data);
        return true;
    } catch (const std::exception& e) {
        // Log error or handle exception
        return false;
    }
}

// Update data
bool DataInterface::updateData(const std::string& symbol, const std::string& date, const std::map<std::string, std::string>& updates) {
    try {
        std::map<std::string, std::string> conditions;
        conditions["symbol"] = symbol;
        conditions["date"] = date;
        
        client->updateData("market_data", "ohlcv", conditions, updates);
        return true;
    } catch (const std::exception& e) {
        // Log error or handle exception
        return false;
    }
}

// Delete data
bool DataInterface::deleteData(const std::string& symbol, const std::string& start_date, const std::string& end_date) {
    try {
        std::map<std::string, std::string> conditions;
        conditions["symbol"] = symbol;
        if (!start_date.empty()) conditions["date >= '" + start_date + "'"] = "";
        if (!end_date.empty()) conditions["date <= '" + end_date + "'"] = "";
        
        client->deleteData("market_data", "ohlcv", conditions);
        return true;
    } catch (const std::exception& e) {
        // Log error or handle exception
        return false;
    }
}
