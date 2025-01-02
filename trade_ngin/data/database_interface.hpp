#ifndef DATABASE_INTERFACE_HPP
#define DATABASE_INTERFACE_HPP

#include <string>
#include <vector>
#include <pqxx/pqxx>

// Struct representing a single row of OHLCV data
struct OHLCV {
    std::string time;   // ISO format date-time as a string
    double open;
    double high;
    double low;
    double close;
    double volume;
    std::string symbol; // Instrument identifier
};

class DatabaseInterface {
public:
    // Constructor
    explicit DatabaseInterface(const std::string& connection_string);

    // Destructor
    ~DatabaseInterface();

    // Methods to query the database
    std::vector<OHLCV> getOHLCVData(
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& symbols = {}
    );

    std::vector<std::string> getSymbols();
    std::string getEarliestDate();
    std::string getLatestDate();
    OHLCV getLatestData(const std::string& symbol);

private:
    pqxx::connection* db_connection;  // Pointer to the database connection
};

#endif // DATABASE_INTERFACE_HPP
