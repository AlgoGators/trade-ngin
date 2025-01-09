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
    std::string endpoint = "/data/" + schema + "/" + table + "?format=" + format;

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
            // Ensure Arrow payload is valid
            if (!payload_arrow) {
                throw std::invalid_argument("Arrow payload is null for insertion.");
            }

            // Serialize the Arrow table to an IPC stream
            std::shared_ptr<arrow::io::BufferOutputStream> stream;
            auto stream_result = arrow::io::BufferOutputStream::Create();
            if (!stream_result.ok()) {
                throw std::runtime_error("Failed to create Arrow BufferOutputStream: " + stream_result.status().ToString());
            }
            stream = stream_result.ValueOrDie();
            auto writer_result = arrow::ipc::MakeFileWriter(stream.get(), payload_arrow->schema());
            if (!writer_result.ok()) {
                throw std::runtime_error("Failed to create Arrow IPC writer: " + writer_result.status().ToString());
            }
            auto writer = writer_result.ValueOrDie();

            // Write the Arrow table to the IPC stream
            auto write_status = writer->WriteTable(*payload_arrow);
            if (!write_status.ok()) {
                throw std::runtime_error("Failed to write Arrow table to IPC stream: " + write_status.ToString());
            }

            // Close the writer
            auto close_status = writer->Close();
            if (!close_status.ok()) {
                throw std::runtime_error("Failed to close Arrow IPC writer: " + close_status.ToString());
            }

            // Finish the stream and get the buffer
            auto buffer_result = stream->Finish();
            if (!buffer_result.ok()) {
                throw std::runtime_error("Failed to finish Arrow IPC stream: " + buffer_result.status().ToString());
            }
            auto buffer = buffer_result.ValueOrDie();

            // Send the Arrow payload as a binary HTTP POST
            client->addHeader("Content-Type: application/octet-stream");
            auto response = client->httpPost(endpoint, std::string(reinterpret_cast<const char*>(buffer->data()), buffer->size()));
            client->clearHeaders();
            // Optionally, handle the response or log it
            std::cout << "Response: " << response << std::endl;
            return true;

        } else {
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


// Update data
bool DataInterface::updateData(const std::string& table, const nlohmann::json& filters, const nlohmann::json& updates) {
    std::string endpoint = "/data/" + table;
    nlohmann::json payload = {{"filters", filters}, {"updates", updates}};
    auto response = client->httpPut(endpoint, payload.dump());
    return true;
}

// Delete data
bool DataInterface::deleteData(const std::string& table, const nlohmann::json& filters) {
    std::string endpoint = "/data/" + table;
    nlohmann::json payload = {{"filters", filters}};
    auto response = client->httpDelete(endpoint, payload.dump());
    return true;
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
