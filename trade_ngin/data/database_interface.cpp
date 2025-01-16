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

// Retrieve OHLCV data as an Apache Arrow Table
std::shared_ptr<arrow::Table> DatabaseInterface::getOHLCVArrowTable(
    const std::string& start_date,
    const std::string& end_date,
    const std::vector<std::string>& symbols
) {
    // Define the schema for the Arrow Table
    auto schema = arrow::schema({
        arrow::field("time", arrow::utf8()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::float64()),
        arrow::field("symbol", arrow::utf8())
    });

    // Create column builders
    arrow::StringBuilder time_builder;
    arrow::DoubleBuilder open_builder, high_builder, low_builder, close_builder, volume_builder;
    arrow::StringBuilder symbol_builder;

    try {
        pqxx::work txn(*db_connection);

        // Build the SQL query
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

        // Execute the query and fetch results
        pqxx::result res = txn.exec(query);

        for (const auto& row : res) {
            // Append each column to its builder
            time_builder.Append(row["time"].c_str());
            open_builder.Append(row["open"].as<double>());
            high_builder.Append(row["high"].as<double>());
            low_builder.Append(row["low"].as<double>());
            close_builder.Append(row["close"].as<double>());
            volume_builder.Append(row["volume"].as<double>());
            symbol_builder.Append(row["symbol"].c_str());
        }

        // Create Arrow Arrays
        std::shared_ptr<arrow::Array> time_array, open_array, high_array, low_array, close_array, volume_array, symbol_array;
        time_builder.Finish(&time_array);
        open_builder.Finish(&open_array);
        high_builder.Finish(&high_array);
        low_builder.Finish(&low_array);
        close_builder.Finish(&close_array);
        volume_builder.Finish(&volume_array);
        symbol_builder.Finish(&symbol_array);

        // Create and return the Arrow Table
        return arrow::Table::Make(schema, {time_array, open_array, high_array, low_array, close_array, volume_array, symbol_array});

    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
}

// Retrieve all unique symbols as an Arrow Table
std::shared_ptr<arrow::Table> DatabaseInterface::getSymbolsAsArrowTable() {
    auto schema = arrow::schema({arrow::field("symbol", arrow::utf8())});
    arrow::StringBuilder symbol_builder;

    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec("SELECT DISTINCT symbol FROM futures_data.ohlcv_1d");

        for (const auto& row : res) {
            symbol_builder.Append(row[0].c_str());
        }

        std::shared_ptr<arrow::Array> symbol_array;
        symbol_builder.Finish(&symbol_array);

        return arrow::Table::Make(schema, {symbol_array});
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
}

// Get the earliest date in the database
std::string DatabaseInterface::getEarliestDate() {
    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec("SELECT MIN(time) FROM futures_data.ohlcv_1d");
        if (res.empty() || res[0][0].is_null()) {
            throw std::runtime_error("No data found in the database");
        }
        return res[0][0].as<std::string>();
    } catch (const std::exception& e) {
        throw std::runtime_error("Error getting earliest date: " + std::string(e.what()));
    }
}

// Get the latest date in the database
std::string DatabaseInterface::getLatestDate() {
    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec("SELECT MAX(time) FROM futures_data.ohlcv_1d");

        if (!res.empty() && !res[0][0].is_null()) {
            return res[0][0].c_str();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
    return "";
}

// Retrieve the latest OHLCV data as an Arrow Table
std::shared_ptr<arrow::Table> DatabaseInterface::getLatestDataAsArrowTable(const std::string& symbol) {
    auto schema = arrow::schema({
        arrow::field("time", arrow::utf8()),
        arrow::field("open", arrow::float64()),
        arrow::field("high", arrow::float64()),
        arrow::field("low", arrow::float64()),
        arrow::field("close", arrow::float64()),
        arrow::field("volume", arrow::float64()),
        arrow::field("symbol", arrow::utf8())
    });

    arrow::StringBuilder time_builder, symbol_builder;
    arrow::DoubleBuilder open_builder, high_builder, low_builder, close_builder, volume_builder;

    try {
        pqxx::work txn(*db_connection);
        std::string query = "SELECT time, open, high, low, close, volume, symbol "
                            "FROM futures_data.ohlcv_1d WHERE symbol = '" + txn.esc(symbol) +
                            "' ORDER BY time DESC LIMIT 1";
        pqxx::result res = txn.exec(query);

        if (!res.empty()) {
            const auto& row = res[0];
            time_builder.Append(row["time"].c_str());
            open_builder.Append(row["open"].as<double>());
            high_builder.Append(row["high"].as<double>());
            low_builder.Append(row["low"].as<double>());
            close_builder.Append(row["close"].as<double>());
            volume_builder.Append(row["volume"].as<double>());
            symbol_builder.Append(row["symbol"].c_str());
        }

        // Create and return the Arrow Table
        std::shared_ptr<arrow::Array> time_array, open_array, high_array, low_array, close_array, volume_array, symbol_array;
        time_builder.Finish(&time_array);
        open_builder.Finish(&open_array);
        high_builder.Finish(&high_array);
        low_builder.Finish(&low_array);
        close_builder.Finish(&close_array);
        volume_builder.Finish(&volume_array);
        symbol_builder.Finish(&symbol_array);

        return arrow::Table::Make(schema, {time_array, open_array, high_array, low_array, close_array, volume_array, symbol_array});

    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query error: ") + e.what());
    }
}

std::vector<std::string> DatabaseInterface::getAllSymbols() {
    std::vector<std::string> symbols;
    try {
        pqxx::work txn(*db_connection);
        pqxx::result result = txn.exec("SELECT DISTINCT symbol FROM futures_data.ohlcv_1d ORDER BY symbol");
        
        for (const auto& row : result) {
            symbols.push_back(row[0].as<std::string>());
        }
        
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "Error getting symbols: " << e.what() << std::endl;
    }
    return symbols;
}
