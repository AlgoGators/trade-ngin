#ifndef DATABASE_INTERFACE_HPP
#define DATABASE_INTERFACE_HPP

#include <string>
#include <vector>
#include <pqxx/pqxx>
#include <arrow/api.h>

class DatabaseInterface {
public:
    // Constructor
    explicit DatabaseInterface(const std::string& connection_string);

    // Destructor
    ~DatabaseInterface();

    // Methods to query the database and return Arrow Tables
    std::shared_ptr<arrow::Table> getOHLCVArrowTable(
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& symbols = {}
    );

    // Get all available symbols
    std::vector<std::string> getAllSymbols();

    // Get symbols as Arrow Table
    std::shared_ptr<arrow::Table> getSymbolsAsArrowTable();

    // Get the earliest date in the database
    std::string getEarliestDate();

    // Get the latest date in the database
    std::string getLatestDate();

    // Get latest data for a symbol
    std::shared_ptr<arrow::Table> getLatestDataAsArrowTable(const std::string& symbol);

private:
    pqxx::connection* db_connection;
};

#endif // DATABASE_INTERFACE_HPP
