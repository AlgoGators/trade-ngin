#ifndef OHLCV_DATA_HANDLER_HPP
#define OHLCV_DATA_HANDLER_HPP

#include <string>
#include <vector>
#include <pqxx/pqxx>
#include <arrow/api.h>
#include "database_client.hpp"

class OHLCVDataHandler {
public:
    // Constructor
    explicit OHLCVDataHandler(const std::string& connection_string);

    // Destructor
    ~OHLCVDataHandler();

    // Methods to query the database and return Arrow Tables
    std::shared_ptr<arrow::Table> getOHLCVArrowTable(
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& symbols = {}
    );

    std::shared_ptr<arrow::Table> getSymbolsAsArrowTable();
    std::string getEarliestDate();
    std::string getLatestDate();
    std::shared_ptr<arrow::Table> getLatestDataAsArrowTable(const std::string& symbol);

private:
    pqxx::connection* db_connection;  // Pointer to the database connection
    std::unique_ptr<DatabaseClient> database_client;  // Database client object
};

#endif // OHLCV_DATA_HANDLER_HPP
