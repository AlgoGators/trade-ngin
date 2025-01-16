#include "ohlcv_data_handler.hpp"
#include "database_client.hpp"
#include <arrow/api.h>
#include <iostream>
#include <memory>
#include <string>


int main() {
    try {
        // Placeholder for connection string details
        std::string db_name = "";
        std::string db_user = "";
        std::string db_password = "";
        std::string db_host = "";
        std::string db_port = "";

        // PostgreSQL connection string
        std::string connection_string = "dbname=" + db_name + " user=" + db_user + " password=" + db_password + " host=" + db_host + " port=" + db_port;

        // Instantiate OHLCVDataHandler and DatabaseClient
        OHLCVDataHandler OHLCVData(connection_string);
        DatabaseClient database_client(connection_string);
        database_client.connect();

        // Fetch OHLCV data as an Arrow Table
        std::cout << "\nFetching the OHLCV data...\n\n";
        auto ohlcv_table = OHLCVData.getOHLCVArrowTable("2024-12-07", "2024-12-19");

        // Print OHLCV data (first 5 rows)
        std::cout << "OHLCV Data (First 5 Rows):\n";
        for (int64_t i = 0; i < std::min<int64_t>(5, ohlcv_table->num_rows()); ++i) {
            std::cout << "Time: " << std::static_pointer_cast<arrow::StringArray>(ohlcv_table->column(0)->chunk(0))->GetString(i)
                      << ", Symbol: " << std::static_pointer_cast<arrow::StringArray>(ohlcv_table->column(1)->chunk(0))->GetString(i)
                      << ", Open: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(2)->chunk(0))->Value(i)
                      << ", High: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(3)->chunk(0))->Value(i)
                      << ", Low: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(4)->chunk(0))->Value(i)
                      << ", Close: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(5)->chunk(0))->Value(i)
                      << ", Volume: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(6)->chunk(0))->Value(i)
                      << "\n";
        }

        // Retrieve unique symbols as an Arrow Table
        std::cout << "\nFetching symbols...\n\n";
        auto symbols_table = OHLCVData.getSymbolsAsArrowTable();

        // Print all symbols
        std::cout << "Symbols:\n";
        for (int64_t i = 0; i < symbols_table->num_rows(); ++i) {
            std::cout << std::static_pointer_cast<arrow::StringArray>(symbols_table->column(0)->chunk(0))->GetString(i) << "\n";
        }

        // Retrieve the earliest and latest dates
        std::cout << "\nFetching earliest and latest dates...\n";
        std::string earliest_date = OHLCVData.getEarliestDate();
        std::string latest_date = OHLCVData.getLatestDate();

        // Print the earliest and latest dates
        std::cout << "\nEarliest date: " << earliest_date << "\n";
        std::cout << "Latest date: " << latest_date << "\n";

        // Fetch metadata using DatabaseClient
        std::cout << "\nFetching metadata...\n";
        auto schemas = database_client.getSchemas();
        auto tables = database_client.getTablesInSchema("futures_data");
        auto columns = database_client.getColumnsInTable("futures_data", "ohlcv_1d");

        // Print schemas
        std::cout << "\nSchemas:\n";
        for (const auto& schema : schemas) {
            std::cout << schema << "\n";
        }

        // Print tables in "futures_data"
        std::cout << "\nTables in futures_data:\n";
        for (const auto& table : tables) {
            std::cout << table << "\n";
        }

        // Print columns in "ohlcv_1d"
        std::cout << "\nColumns in ohlcv_1d:\n";
        for (const auto& [column_name, data_type] : columns) {
            std::cout << column_name << " (" << data_type << ")\n";
