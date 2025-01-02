#include "database_interface.hpp"
#include <iostream>
#include <stdexcept>

// Constructor
DatabaseInterface::DatabaseInterface(const std::string& connection_string) {
    try {
        db_connection = new pqxx::connection(connection_string);
        if (!db_connection->is_open()) {
            throw std::runtime_error("Failed to connect to the database.");
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Connection error: ") + e.what());
    }
}

// Destructor
DatabaseInterface::~DatabaseInterface() {
    if (db_connection) {
        db_connection->close();
        delete db_connection;
    }
}

// Retrieve OHLCV data
std::vector<OHLCV> DatabaseInterface::getOHLCVData(
    const std::string& start_date,
    const std::string& end_date,
    const std::vector<std::string>& symbols
) {
    std::vector<OHLCV> result;
    try {
        pqxx::work txn(*db_connection);
        std::string query = "SELECT time, open, high, low, close, volume, symbol "
                            "FROM futures_data.ohlcv_1d WHERE time BETWEEN '" + start_date + 
                            "' AND '" + end_date + "'";
        if (!symbols.empty()) {
            query += " AND symbol IN (";
            for (size_t i = 0; i < symbols.size(); ++i) {
                query += "'" + txn.esc(symbols[i]) + "'";
                if (i < symbols.size() - 1) query += ", ";
            }
            query += ")";
        }
        query += " ORDER BY symbol, time";

        pqxx::result res = txn.exec(query);
        for (const auto& row : res) {
            OHLCV record {
                row["time"].as<std::string>(),
                row["open"].as<double>(),
                row["high"].as<double>(),
                row["low"].as<double>(),
                row["close"].as<double>(),
                row["volume"].as<double>(),
                row["symbol"].as<std::string>()
            };
            result.push_back(record);
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
    return result;
}

// Retrieve all unique symbols
std::vector<std::string> DatabaseInterface::getSymbols() {
    std::vector<std::string> symbols;
    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec("SELECT DISTINCT symbol FROM futures_data.ohlcv_1d");

        for (const auto& row : res) {
            symbols.push_back(row[0].as<std::string>());
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
    return symbols;
}

// Get the earliest date in the database
std::string DatabaseInterface::getEarliestDate() {
    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec("SELECT MIN(time) FROM futures_data.ohlcv_1d");

        if (!res.empty() && !res[0][0].is_null()) {
            return res[0][0].as<std::string>();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
    return "";
}

// Get the latest date in the database
std::string DatabaseInterface::getLatestDate() {
    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec("SELECT MAX(time) FROM futures_data.ohlcv_1d");

        if (!res.empty() && !res[0][0].is_null()) {
            return res[0][0].as<std::string>();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
    return "";
}

// Retrieve the latest OHLCV data for a specific symbol
OHLCV DatabaseInterface::getLatestData(const std::string& symbol) {
    try {
        pqxx::work txn(*db_connection);
        std::string query = "SELECT time, open, high, low, close, volume, symbol "
                            "FROM futures_data.ohlcv_1d WHERE symbol = '" + txn.esc(symbol) + 
                            "' ORDER BY time DESC LIMIT 1";
        pqxx::result res = txn.exec(query);

        if (!res.empty()) {
            return {
                res[0]["time"].as<std::string>(),
                res[0]["open"].as<double>(),
                res[0]["high"].as<double>(),
                res[0]["low"].as<double>(),
                res[0]["close"].as<double>(),
                res[0]["volume"].as<double>(),
                res[0]["symbol"].as<std::string>()
            };
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
    throw std::runtime_error("No data found for symbol: " + symbol);
}
