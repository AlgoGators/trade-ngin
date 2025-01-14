#include "ohlcv_data_handler.hpp"
#include <iostream>
#include <stdexcept>

// Constructor
OHLCVDataHandler::OHLCVDataHandler(const std::string& connection_string)
    : database_client(std::make_unique<DatabaseClient>(connection_string)) {
    try {
        database_client->connect();
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to initialize OHLCVDataHandler: " + std::string(e.what()));
    }
}

// Destructor
OHLCVDataHandler::~OHLCVDataHandler() {
    database_client->disconnect();
}

// Retrieve OHLCV data as an Apache Arrow Table
std::shared_ptr<arrow::Table> OHLCVDataHandler::getOHLCVArrowTable(
    const std::string& start_date,
    const std::string& end_date,
    const std::vector<std::string>& symbols
) {
    // Define the schema for the Arrow Table
    auto schema = arrow::schema({
        arrow::field("time", arrow::utf8()),
        arrow::field("symbol", arrow::utf8()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::float64())
    });

    // Build the SQL query
    std::string query = "SELECT * FROM futures_data.ohlcv_1d ";
    query += "WHERE time BETWEEN '" + start_date + "' AND '" + end_date + "'";

    if (!symbols.empty()) {
        query += " AND symbol IN (";
        for (int i = 0; i < symbols.size(); ++i) {
            query += "'" + symbols[i] + "'";
            if (i < symbols.size() - 1) query += ", ";
        }
        query += ")";
    }

    query += " ORDER BY symbol, time;";

    try {
        return database_client->fetchDataAsArrowTable(query, schema);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error fetching OHLCV data: " + std::string(e.what()));
    }
}

std::shared_ptr<arrow::Table> OHLCVDataHandler::getSymbolsAsArrowTable() {
    auto schema = arrow::schema({arrow::field("symbol", arrow::utf8())});

    std::string query = R"(
        WITH latest_symbols AS (
            SELECT DISTINCT ON (symbol) symbol
            FROM futures_data.ohlcv_1d
            ORDER BY symbol, time DESC
        )
        SELECT symbol
        FROM latest_symbols
        ORDER BY symbol;
    )";

    try {        
        auto result = database_client->fetchDataAsArrowTable(query, schema);
        
        // Validate the result
        if (!result) {
            throw std::runtime_error("Null Arrow table returned from database");
        }
        
        if (result->num_columns() != 1) {
            throw std::runtime_error(
                "Unexpected number of columns in result: " + 
                std::to_string(result->num_columns())
            );
        }
        
        return result;
        
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Error fetching symbols from OHLCV data: " + std::string(e.what())
        );
    }
}

// Get the earliest date in the database
std::string OHLCVDataHandler::getEarliestDate() {
    std::string query = "SELECT MIN(time) AS earliest_time FROM futures_data.ohlcv_1d";

    try {
        auto result = database_client->executeQuery(query);
        if (!result.empty() && !result[0]["earliest_time"].is_null()) {
            return result[0]["earliest_time"].c_str();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Error fetching earliest date: " + std::string(e.what()));
    }

    return "";
}

// Get the latest date in the database
std::string OHLCVDataHandler::getLatestDate() {
    std::string query = "SELECT MAX(time) AS latest_time FROM futures_data.ohlcv_1d";

    try {
        auto result = database_client->executeQuery(query);
        if (!result.empty() && !result[0]["latest_time"].is_null()) {
            return result[0]["latest_time"].c_str();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error("Error fetching latest date: " + std::string(e.what()));
    }

    return "";
}

// Retrieve the latest OHLCV data as an Arrow Table
std::shared_ptr<arrow::Table> OHLCVDataHandler::getLatestDataAsArrowTable(const std::string& symbol) {
    auto schema = arrow::schema({
        arrow::field("time", arrow::utf8()),
        arrow::field("symbol", arrow::utf8()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::float64()),
    });

    std::string query = "SELECT time, open, high, low, close, volume, symbol FROM futures_data.ohlcv_1d ";
    query += "WHERE symbol = '" + symbol + "' ORDER BY time DESC LIMIT 1";

    try {
        return database_client->fetchDataAsArrowTable(query, schema);
    } catch (const std::exception& e) {
        throw std::runtime_error("Error fetching latest OHLCV data: " + std::string(e.what()));
    }
}
