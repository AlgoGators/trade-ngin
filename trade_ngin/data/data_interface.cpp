#define NOMINMAX
#include "data_interface.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/ipc/writer.h>
#include <arrow/ipc/reader.h>
#include <stdexcept>
#include <iostream>

// Constructor
DataInterface::DataInterface(std::shared_ptr<ApiClient> client) : client(std::move(client)) {}

// Retrieve OHLCV data
std::shared_ptr<arrow::Table> DataInterface::getOHLCV(
    const std::string& start_date,
    const std::string& end_date,
    const std::vector<std::string>& symbols
) {
    std::string endpoint = "/data/futures_data/ohlcv_1d?format=arrow";
    endpoint += "&range_filters={\"time\":{\"gte\":\"" + start_date + "\",\"lte\":\"" + end_date + "\"}}";

    if (!symbols.empty()) {
        std::string symbols_json = "";
        for (size_t i = 0; i < symbols.size(); ++i) {
            symbols_json += "\"" + symbols[i] + "\"";
            if (i < symbols.size() - 1) symbols_json += ",";
        }
        endpoint += "&filters={\"symbol\":" + symbols_json + "}";
    }

    client->addHeader("Content-Type: application/json");
    auto response = client->httpGet(endpoint);
    client->clearHeaders();

    // Parse Arrow Table
    auto buffer = arrow::Buffer::FromString(response);
    arrow::io::BufferReader reader(buffer);
    auto batch_reader_result = arrow::ipc::RecordBatchFileReader::Open(&reader);
    if (!batch_reader_result.ok()) {
        throw std::runtime_error("Failed to open Arrow RecordBatchFileReader: " + batch_reader_result.status().ToString());
    }
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> batch_reader = batch_reader_result.ValueOrDie();

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int i = 0; i < batch_reader->num_record_batches(); ++i) {
        auto batch_result = batch_reader->ReadRecordBatch(i);
        if (!batch_result.ok()) {
            throw std::runtime_error("Failed to read record batch: " + batch_result.status().ToString());
        }
        batches.push_back(batch_result.ValueOrDie());
    }

    // Combine all record batches into a single table
    auto table_result = arrow::Table::FromRecordBatches(batches);
    if (!table_result.ok()) {
        throw std::runtime_error("Failed to combine record batches into a table: " + table_result.status().ToString());
    }
    return table_result.ValueOrDie();
}

// Retrieve symbols
std::shared_ptr<arrow::Table> DataInterface::getSymbols() {
    // Updated endpoint to fetch distinct symbols from the OHLCV table
    std::string endpoint = "/data/futures_data/ohlcv_1d?format=arrow&columns=symbol&distinct=true";
    client->addHeader("Content-Type: application/json");
    auto response = client->httpGet(endpoint);
    client->clearHeaders();

    // Parse Arrow Table
    auto buffer = arrow::Buffer::FromString(response);
    arrow::io::BufferReader reader(buffer);

    auto batch_reader_result = arrow::ipc::RecordBatchFileReader::Open(&reader);
    if (!batch_reader_result.ok()) {
        throw std::runtime_error("Failed to open Arrow RecordBatchFileReader: " + batch_reader_result.status().ToString());
    }
    std::shared_ptr<arrow::ipc::RecordBatchFileReader> batch_reader = batch_reader_result.ValueOrDie();

    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int i = 0; i < batch_reader->num_record_batches(); ++i) {
        auto batch_result = batch_reader->ReadRecordBatch(i);
        if (!batch_result.ok()) {
            throw std::runtime_error("Failed to read record batch: " + batch_result.status().ToString());
        }
        batches.push_back(batch_result.ValueOrDie());
    }

    auto table_result = arrow::Table::FromRecordBatches(batches);
    if (!table_result.ok()) {
        throw std::runtime_error("Failed to combine record batches into a table: " + table_result.status().ToString());
    }
    return table_result.ValueOrDie();
}

// Insert data using JSON or Apache Arrow
bool DataInterface::insertData(const std::string& schema, const std::string& table, const std::string& format, const nlohmann::json& payload_json, const std::shared_ptr<arrow::Table>& payload_arrow) {
    // Construct the endpoint with the format query parameter
    std::string endpoint = "/data/" + schema + "/" + table + "?format=json";

    try {
        if (format == "json") {
            // Ensure JSON payload is valid
            if (payload_json.empty()) {
                throw std::invalid_argument("JSON payload is empty for insertion.");
            }

            // Send JSON payload via HTTP POST
            client->addHeader("Content-Type: application/json");
            auto response = client->httpPost(endpoint, payload_json.dump());
            client->clearHeaders();
            // Optionally, handle the response or log it
            std::cout << "Response: " << response << std::endl;
            return true;

        } else if (format == "arrow") {
            if (!payload_arrow) {
                throw std::invalid_argument("Arrow payload is null for insertion.");
            }

            // Convert Arrow table to JSON
            nlohmann::json json_payload = nlohmann::json::array();

            for (int64_t i = 0; i < payload_arrow->num_rows(); ++i) {
                nlohmann::json row = nlohmann::json::object();
                for (int j = 0; j < payload_arrow->num_columns(); ++j) {
                    auto column = payload_arrow->column(j);
                    auto array = column->chunk(0);

                    // Handle different Arrow data types
                    if (array->type_id() == arrow::Type::STRING) {
                        auto str_array = std::static_pointer_cast<arrow::StringArray>(array);
                        row[payload_arrow->schema()->field(j)->name()] = str_array->GetString(i);
                    } else if (array->type_id() == arrow::Type::DOUBLE) {
                        auto dbl_array = std::static_pointer_cast<arrow::DoubleArray>(array);
                        row[payload_arrow->schema()->field(j)->name()] = dbl_array->Value(i);
                    } else if (array->type_id() == arrow::Type::INT64) {
                        auto int_array = std::static_pointer_cast<arrow::Int64Array>(array);
                        row[payload_arrow->schema()->field(j)->name()] = int_array->Value(i);
                    } else {
                        throw std::runtime_error("Unsupported Arrow data type: " + array->type()->ToString());
                    }
                }
                json_payload.push_back(row);
            }

            // Construct the endpoint
            std::string endpoint = "/data/" + schema + "/" + table + "?format=json";

            // Send the JSON payload via HTTP POST
            client->addHeader("Content-Type: application/json");
            auto response = client->httpPost(endpoint, json_payload.dump());
            client->clearHeaders();

            // Optionally log the response
            std::cout << "Response: " << response << std::endl;

            return true;
        }
        else {
            throw std::invalid_argument("Unsupported format: " + format);
        }
    } catch (const std::exception& e) {
        std::cerr << "Error inserting data: " << e.what() << std::endl;
        return false;
    }
}

/*
NEED TO UPDATE updateData AND deleteData FUNCTIONS
TO ADD CLIENT HEADERS
*/


bool DataInterface::updateData(
    const std::string& schema,
    const std::string& table,
    const nlohmann::json& filters,
    const nlohmann::json& updates
) {
    if (filters.empty()) {
        throw std::invalid_argument("Filters cannot be empty for update.");
    }

    if (updates.empty()) {
        throw std::invalid_argument("Updates cannot be empty for update.");
    }

    // Construct the endpoint
    std::string endpoint = "/data/" + schema + "/" + table;

    // Construct the payload
    nlohmann::json payload = {
        {"filters", filters},
        {"updates", updates}
    };

    try {
        // Add headers
        client->addHeader("Content-Type: application/json");

        // Perform the HTTP PUT request
        auto response = client->httpPut(endpoint, payload.dump());
        client->clearHeaders();

        // Log the response
        std::cout << "Update response: " << response << std::endl;

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error updating data: " << e.what() << std::endl;
        return false;
    }
}


// Delete data
bool DataInterface::deleteData(
    const std::string& schema,
    const std::string& table,
    const nlohmann::json& filters
) {
    if (filters.empty()) {
        throw std::invalid_argument("Filters cannot be empty for deletion.");
    }

    // Construct the endpoint
    std::string endpoint = "/data/" + schema + "/" + table;

    // Construct the payload
    nlohmann::json payload = {{"filters", filters}};

    try {
        // Add headers
        client->addHeader("Content-Type: application/json");

        // Perform the HTTP DELETE request
        auto response = client->httpDelete(endpoint, payload.dump());
        client->clearHeaders();

        // Log the response
        std::cout << "Delete response: " << response << std::endl;

        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error deleting data: " << e.what() << std::endl;
        return false;
    }
}


// Retrieve earliest date
std::string DataInterface::getEarliestDate() {
    // Updated endpoint to query the minimum time
    std::string endpoint = "/data/futures_data/ohlcv_1d?format=json&columns=time&aggregations={\"time\":\"MIN\"}";
    auto response = client->httpGet(endpoint);

    // Parse the JSON response
    auto json_response = nlohmann::json::parse(response);
    if (json_response.empty()) {
        throw std::runtime_error("No data returned for earliest date.");
    }

    return json_response[0]["time"];
}

// Retrieve latest date
std::string DataInterface::getLatestDate() {
    // Updated endpoint to query the maximum time
    std::string endpoint = "/data/futures_data/ohlcv_1d?format=json&columns=time&aggregations={\"time\":\"MAX\"}";
    auto response = client->httpGet(endpoint);

    // Parse the JSON response
    auto json_response = nlohmann::json::parse(response);
    if (json_response.empty()) {
        throw std::runtime_error("No data returned for latest date.");
    }

    return json_response[0]["time"];
}
