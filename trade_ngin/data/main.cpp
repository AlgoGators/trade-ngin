#include "database_interface.hpp"
#include <arrow/api.h>
#include <iostream>
#include <memory>

int main() {
    try {
        // Replace with your actual PostgreSQL connection string
        const std::string connection_string = "dbname= user= password= hostaddr= port=";

        // Instantiate the DatabaseInterface
        DatabaseInterface dbInterface(connection_string);

        // Fetch OHLCV data as an Arrow Table
        auto ohlcv_table = dbInterface.getOHLCVArrowTable("2010-06-07", "2024-12-19");
        auto ohlcv_MES_table = dbInterface.getOHLCVArrowTable("2010-06-07", "2024-12-19", {"MES.c.0"});

        // Retrieve unique symbols as an Arrow Table
        auto symbols_table = dbInterface.getSymbolsAsArrowTable();

        // Retrieve the earliest and latest dates
        std::string earliest_date = dbInterface.getEarliestDate();
        std::string latest_date = dbInterface.getLatestDate();

        // Print results
        std::cout << "Symbols:\n";
        auto symbol_array = std::static_pointer_cast<arrow::StringArray>(symbols_table->column(0)->chunk(0));
        for (int64_t i = 0; i < symbol_array->length(); ++i) {
            std::cout << symbol_array->GetString(i) << "\n";
        }

        std::cout << "\nEarliest Date: " << earliest_date << "\n";
        std::cout << "Latest Date: " << latest_date << "\n";

        // Print OHLCV Data (First 5 Rows)
        std::cout << "\nOHLCV Data (First 5 Rows):\n";
        for (int64_t i = 0; i < std::min<int64_t>(5, ohlcv_table->num_rows()); ++i) {
            std::cout << "Time: " << std::static_pointer_cast<arrow::StringArray>(ohlcv_table->column(0)->chunk(0))->GetString(i)
                      << ", Open: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(1)->chunk(0))->Value(i)
                      << ", High: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(2)->chunk(0))->Value(i)
                      << ", Low: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(3)->chunk(0))->Value(i)
                      << ", Close: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(4)->chunk(0))->Value(i)
                      << ", Volume: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_table->column(5)->chunk(0))->Value(i)
                      << ", Symbol: " << std::static_pointer_cast<arrow::StringArray>(ohlcv_table->column(6)->chunk(0))->GetString(i)
                      << "\n";
        }

        std::cout << "\nOHLCV Data for MES.c.0 (First 5 Rows):\n";
        for (int64_t i = 0; i < std::min<int64_t>(5, ohlcv_MES_table->num_rows()); ++i) {
            std::cout << "Time: " << std::static_pointer_cast<arrow::StringArray>(ohlcv_MES_table->column(0)->chunk(0))->GetString(i)
                      << ", Open: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_MES_table->column(1)->chunk(0))->Value(i)
                      << ", High: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_MES_table->column(2)->chunk(0))->Value(i)
                      << ", Low: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_MES_table->column(3)->chunk(0))->Value(i)
                      << ", Close: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_MES_table->column(4)->chunk(0))->Value(i)
                      << ", Volume: " << std::static_pointer_cast<arrow::DoubleArray>(ohlcv_MES_table->column(5)->chunk(0))->Value(i)
                      << ", Symbol: " << std::static_pointer_cast<arrow::StringArray>(ohlcv_MES_table->column(6)->chunk(0))->GetString(i)
                      << "\n";
        }

    } catch (const std::exception& e) { 
        // Handle any exceptions that occur
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
