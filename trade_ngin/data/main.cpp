#include "database_interface.hpp"
#include <iostream>
#include <vector>
#include <iomanip> // For formatted output

int main() {
    try {
        // Replace with your actual PostgreSQL connection string
        const std::string connection_string = "dbname= user= password= hostaddr= port=";

        // Instantiate the DatabaseInterface
        DatabaseInterface dbInterface(connection_string);

        // Fetch OHLCV data for a date range
        std::vector<OHLCV> ohlcv_data = dbInterface.getOHLCVData("2010-06-07", "2024-12-19");
        std::vector<OHLCV> ohlcv_6B_data = dbInterface.getOHLCVData("2010-06-07", "2024-12-19", {"6B.c.0"});

        // Retrieve unique symbols
        std::vector<std::string> symbols = dbInterface.getSymbols();

        // Retrieve the earliest and latest dates
        std::string earliest_date = dbInterface.getEarliestDate();
        std::string latest_date = dbInterface.getLatestDate();

        // Print results
        std::cout << "Symbols:\n";
        for (const auto& symbol : symbols) {
            std::cout << symbol << "\n";
        }

        std::cout << "\nEarliest Date: " << earliest_date << "\n";
        std::cout << "Latest Date: " << latest_date << "\n";

        std::cout << "\nOHLCV Data (First 5 Rows):\n";
        for (size_t i = 0; i < std::min<size_t>(5, ohlcv_data.size()); ++i) {
            const auto& row = ohlcv_data[i];
            std::cout << "Time: " << row.time
                      << ", Open: " << row.open
                      << ", High: " << row.high
                      << ", Low: " << row.low
                      << ", Close: " << row.close
                      << ", Volume: " << row.volume
                      << ", Symbol: " << row.symbol << "\n";
        }

        std::cout << "\nOHLCV Data for 6B.c.0 (First 5 Rows):\n";
        for (size_t i = 0; i < std::min<size_t>(5, ohlcv_6B_data.size()); ++i) {
            const auto& row = ohlcv_6B_data[i];
            std::cout << "Time: " << row.time
                      << ", Open: " << row.open
                      << ", High: " << row.high
                      << ", Low: " << row.low
                      << ", Close: " << row.close
                      << ", Volume: " << row.volume
                      << ", Symbol: " << row.symbol << "\n";
        }

    } catch (const std::exception& e) {
        // Handle any exceptions that occur
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
