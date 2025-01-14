#include <iostream>
#include "data_client_database.hpp"
#include <arrow/api.h>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace {
    // Helper function to convert time_point to SQL date string
    std::string timePointToSQLDate(const std::chrono::system_clock::time_point& tp) {
        auto time = std::chrono::system_clock::to_time_t(tp);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d");
        return ss.str();
    }

    // Helper function to parse SQL date string to time_point
    std::chrono::system_clock::time_point SQLDateToTimePoint(const std::string& date) {
        std::tm tm = {};
        std::stringstream ss(date);
        ss >> std::get_time(&tm, "%Y-%m-%d");
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }
}

DatabaseDataClient::DatabaseDataClient() 
    : db_(std::make_unique<DatabaseInterface>()) {
    try {
        // Verify connection on startup
        auto earliest = db_->getEarliestDate();
        auto latest = db_->getLatestDate();
        std::cout << "Database connected successfully. Data range: " 
                  << earliest << " to " << latest << "\n";
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to initialize DatabaseDataClient: " + 
                               std::string(e.what()));
    }
}

std::optional<DataClient::DatasetRange> DatabaseDataClient::get_dataset_range(Dataset ds) {
    try {
        auto earliest = db_->getEarliestDate();
        auto latest = db_->getLatestDate();
        
        return DatasetRange{
            SQLDateToTimePoint(earliest),
            SQLDateToTimePoint(latest)
        };
    } catch (const std::exception& e) {
        std::cerr << "Error getting dataset range: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::optional<DataFrame> DatabaseDataClient::get_contract_data(
    Dataset ds,
    const std::string& symbol,
    Agg agg,
    RollType roll_type,
    ContractType contract_type,
    std::chrono::system_clock::time_point start,
    std::chrono::system_clock::time_point end
) {
    try {
        // Convert time_points to SQL date strings
        std::string start_date = timePointToSQLDate(start);
        std::string end_date = timePointToSQLDate(end);

        // Fetch data from database
        auto arrow_table = db_->getOHLCVArrowTable(start_date, end_date, {symbol});
        if (!arrow_table || arrow_table->num_rows() == 0) {
            std::cerr << "No data found for symbol " << symbol 
                      << " between " << start_date << " and " << end_date << "\n";
            return std::nullopt;
        }

        DataFrame df;
        df.fromArrowTable(arrow_table);
        return df;

    } catch (const std::exception& e) {
        std::cerr << "Error fetching contract data: " << e.what() << "\n";
        return std::nullopt;
    }
}

std::optional<DataFrame> DatabaseDataClient::get_definitions(Dataset ds, const DataFrame& data) {
    try {
        // TODO: Implement fetching contract definitions if needed
        return DataFrame{};
    } catch (const std::exception& e) {
        std::cerr << "Error fetching definitions: " << e.what() << "\n";
        return std::nullopt;
    }
} 