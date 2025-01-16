#include "data_interface.hpp"
#include "api_client.hpp"
#include <iostream>
#include <memory>
#include <arrow/api.h>

int main() {
    try {
        // Replace these with your actual API base URL and API key
        const std::string api_base_url = "http://127.0.0.1:8000";
        const std::string api_key = "dVeoHEJv-h8fLivoMC2ySfCGDUW9grI-0X7VHrHoNN4";

        // Instantiate the ApiClient
        auto api_client = std::make_shared<ApiClient>(api_base_url, api_key);

        // Instantiate the DataInterface
        DataInterface data_interface(api_client);

        // Fetch OHLCV data as an Apache Arrow Table
        std::cout << "Fetching OHLCV data...\n";
        auto ohlcv_table = data_interface.getOHLCV("2023-01-01", "2023-12-31", {"MES.c.0"});

        // Print the first 5 rows of the OHLCV table
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

        // Fetch all unique symbols
        std::cout << "\nFetching unique symbols...\n";
        auto symbols_table = data_interface.getSymbols();
        auto symbol_array = std::static_pointer_cast<arrow::StringArray>(symbols_table->column(0)->chunk(0));
        std::cout << "Unique Symbols:\n";
        for (int64_t i = 0; i < symbol_array->length(); ++i) {
            std::cout << symbol_array->GetString(i) << "\n";
        }
    
        // Fetch the earliest and latest dates
        std::cout << "\nFetching earliest and latest dates...\n";
        std::string earliest_date = data_interface.getEarliestDate();
        std::string latest_date = data_interface.getLatestDate();
        std::cout << "Earliest Date: " << earliest_date << "\n";
        std::cout << "Latest Date: " << latest_date << "\n";
        

        // Insert new data using JSON
        std::cout << "\nInserting new data using JSON...\n";
        nlohmann::json new_data = {
            {{"time", "2015-01-01T00:00:00Z"}, {"symbol", "bop"}, {"open", 100.0}, {"high", 105.0}, {"low", 95.0}, {"close", 102.0}, {"volume", 10000.0}}
        };
        bool insert_success = data_interface.insertData("strategies", "test", "json", new_data, nullptr);
        std::cout << "Insert Success: " << (insert_success ? "Yes" : "No") << "\n";
    
        // Insert new data using Arrow
        std::cout << "\nInserting new data using Arrow...\n";
        // Define Arrow schema
        auto schema = arrow::schema({
            arrow::field("time", arrow::utf8()),
            arrow::field("symbol", arrow::utf8()),
            arrow::field("open", arrow::float64()),
            arrow::field("high", arrow::float64()),
            arrow::field("low", arrow::float64()),
            arrow::field("close", arrow::float64()),
            arrow::field("volume", arrow::int64())
        });

        // Create Arrow builders
        arrow::StringBuilder time_builder, symbol_builder;
        arrow::DoubleBuilder open_builder, high_builder, low_builder, close_builder;
        arrow::Int64Builder volume_builder;

        // Append data
        time_builder.Append("2025-01-01T00:00:00Z");
        symbol_builder.Append("darn");
        open_builder.Append(100.0);
        high_builder.Append(105.0);
        low_builder.Append(95.0);
        close_builder.Append(102.0);
        volume_builder.Append(10000.0);

        // Create Arrow arrays
        std::shared_ptr<arrow::Array> time_array, symbol_array, open_array, high_array, low_array, close_array, volume_array;
        time_builder.Finish(&time_array);
        symbol_builder.Finish(&symbol_array);
        open_builder.Finish(&open_array);
        high_builder.Finish(&high_array);
        low_builder.Finish(&low_array);
        close_builder.Finish(&close_array);
        volume_builder.Finish(&volume_array);

        // Create Arrow table
        auto table = arrow::Table::Make(schema, {time_array, symbol_array, open_array, high_array, low_array, close_array, volume_array});

        // Insert Arrow data
        bool success_arrow = data_interface.insertData("strategies", "test", "arrow", nlohmann::json::object(), table);
        if (success_arrow) {
            std::cout << "Arrow data inserted successfully." << std::endl;
        }

        // Update existing data
        std::cout << "\nUpdating data...\n";
        // Define filters and updates
        nlohmann::json filters = {{"symbol", "darn"}, {"time", "2025-01-01T00:00:00Z"}};
        nlohmann::json updates = {{"open", 101.0}, {"close", 103.0}};

        // Perform the update
        bool success = data_interface.updateData("strategies", "test", filters, updates);
        if (success) {
            std::cout << "Update successful." << std::endl;
        }

        // Delete data
        std::cout << "\nDeleting data...\n";
        // Define filters for deletion
        nlohmann::json filters = {{"symbol", "darn"}, {"time", "2025-01-01T00:00:00Z"}};

        // Perform the deletion
        bool success = data_interface.deleteData("strategies", "test", filters);
        if (success) {
            std::cout << "Deletion successful." << std::endl;
        }

    } catch (const std::exception& e) {
        // Handle any exceptions that occur
        std::cerr << "Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }
    

    return EXIT_SUCCESS;
}
