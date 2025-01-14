#include <iostream>
#include <iomanip>
#include <chrono>
#include "data_client_database.hpp"

void printDataFrameInfo(const DataFrame& df) {
    std::cout << "\nDataFrame Info:\n";
    std::cout << "Rows: " << df.rows() << "\n";
    
    auto cols = df.columns();
    std::cout << "Columns (" << cols.size() << "): ";
    for (const auto& col : cols) {
        std::cout << col << " ";
    }
    std::cout << "\n\n";

    // Print first few rows
    const size_t MAX_ROWS = 5;
    std::cout << std::fixed << std::setprecision(2);
    
    for (size_t i = 0; i < std::min(MAX_ROWS, df.rows()); ++i) {
        std::cout << "Row " << i << ":\n";
        for (const auto& col : cols) {
            auto values = df.get_column(col);
            std::cout << "  " << std::setw(10) << col << ": " << values[i] << "\n";
        }
        std::cout << "\n";
    }
}

int main() {
    try {
        std::cout << "Initializing DatabaseDataClient...\n";
        auto client = std::make_unique<DatabaseDataClient>();

        // Get available date range
        auto range = client->get_dataset_range(Dataset::CME);
        if (!range) {
            std::cerr << "Failed to get dataset range\n";
            return 1;
        }

        // Test with ES futures data
        const std::string symbol = "ES";
        auto start = range->start;
        auto end = range->end;

        std::cout << "Fetching data for " << symbol << "...\n";
        auto df = client->get_contract_data(
            Dataset::CME,
            symbol,
            Agg::DAILY,
            RollType::CALENDAR,
            ContractType::FRONT,
            start,
            end
        );

        if (!df) {
            std::cerr << "Failed to get contract data\n";
            return 1;
        }

        printDataFrameInfo(*df);

        std::cout << "\nTest completed successfully!\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
} 