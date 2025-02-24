#include "../data/database_client.hpp"
#include <spdlog/spdlog.h>
#include <pqxx/pqxx>

DatabaseClient::DatabaseClient(const std::string& conn_string) 
    : connection_string(conn_string), db_connection(nullptr) {
    connect();
}

DatabaseClient::~DatabaseClient() {
    disconnect();
}

void DatabaseClient::connect() {
    try {
        if (!db_connection) {
            db_connection = new pqxx::connection(connection_string);
            spdlog::info("Successfully connected to database");
        }
    } catch (const std::exception& e) {
        spdlog::error("Failed to connect to database: {}", e.what());
        throw;
    }
}

void DatabaseClient::disconnect() {
    if (db_connection) {
        db_connection->close();
        delete db_connection;
        db_connection = nullptr;
    }
}

pqxx::result DatabaseClient::executeQuery(const std::string& query) {
    try {
        if (!db_connection) {
            connect();
        }
        pqxx::work txn(*db_connection);
        pqxx::result result = txn.exec(query);
        txn.commit();
        return result;
    } catch (const std::exception& e) {
        spdlog::error("Failed to execute query: {}", e.what());
        throw;
    }
}
