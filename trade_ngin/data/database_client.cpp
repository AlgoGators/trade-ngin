#include "database_client.hpp"
#include <iostream>
#include <stdexcept>

// Constructor
DatabaseClient::DatabaseClient(const std::string& connection_string) 
    : connection_string(connection_string), db_connection(nullptr) {}

// Destructor
DatabaseClient::~DatabaseClient() {
    disconnect();
}

// Establish a connection to the database
void DatabaseClient::connect() {
    if (db_connection && db_connection->is_open()) {
        std::cerr << "Database connection already established." << std::endl;
        return;
    }

    try {
        db_connection = new pqxx::connection(connection_string);
        if (!db_connection->is_open()) {
            throw std::runtime_error("Failed to connect to the database.");
        }
        std::cout << "Database connection established successfully." << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Connection error: ") + e.what());
    }
}

// Close the database connection
void DatabaseClient::disconnect() {
    if (db_connection) {
        try {
            db_connection->close();
            delete db_connection;
            db_connection = nullptr;
            std::cout << "Database connection closed successfully." << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error while closing the database connection: " << e.what() << std::endl;
        }
    }
}

// Execute a raw SQL query
pqxx::result DatabaseClient::executeQuery(const std::string& query) {
    if (!db_connection || !db_connection->is_open()) {
        throw std::runtime_error("Database connection is not open.");
    }

    try {
        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec(query);
        txn.commit();
        return res;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Query execution error: ") + e.what());
    }
}

std::shared_ptr<arrow::Table> DatabaseClient::fetchDataAsArrowTable(
    const std::string& query, const std::shared_ptr<arrow::Schema>& schema
) {
    if (!db_connection || !db_connection->is_open()) {
        throw std::runtime_error("Database connection is not open.");
    }

    // Initialize Arrow builders based on schema
    std::vector<std::shared_ptr<arrow::ArrayBuilder>> builders;
    std::vector<std::shared_ptr<arrow::StringBuilder>> string_builders;
    std::vector<std::shared_ptr<arrow::DoubleBuilder>> double_builders;

    for (const auto& field : schema->fields()) {
        if (field->type()->id() == arrow::Type::STRING) {
            auto builder = std::make_shared<arrow::StringBuilder>();
            builders.push_back(builder);
            string_builders.push_back(builder);
            double_builders.push_back(nullptr);
        } else if (field->type()->id() == arrow::Type::DOUBLE) {
            auto builder = std::make_shared<arrow::DoubleBuilder>();
            builders.push_back(builder);
            double_builders.push_back(builder);
            string_builders.push_back(nullptr);
        } else {
            throw std::runtime_error("Unsupported Arrow field type.");
        }
    }

    try {
        auto start_time = std::chrono::high_resolution_clock::now();

        pqxx::work txn(*db_connection);
        pqxx::result res = txn.exec(query);

        auto query_time = std::chrono::high_resolution_clock::now();
        std::cout << "Query execution time: " 
                  << std::chrono::duration_cast<std::chrono::milliseconds>(query_time - start_time).count() 
                  << " ms\n" << std::endl;

        // Check if result is empty
        if (res.empty()) {
            throw std::runtime_error("Query returned no data.");
        }

        // Validate schema-field alignment
        if (schema->num_fields() != res.columns()) {
            throw std::runtime_error(
                "Schema field count (" + std::to_string(schema->num_fields()) + 
                ") does not match query result column count (" + std::to_string(res.columns()) + ")."
            );
        }

        // Populate builders with query results using index-based iteration
        size_t row_count = 0;
        for (size_t row_idx = 0; row_idx < res.size(); ++row_idx) {
            const auto& row = res[row_idx];
            ++row_count;

            // Sanity check to prevent infinite loops
            if (row_count > res.size()) {
                throw std::runtime_error("Row count exceeds expected number of rows");
            }

            // Process each field in the row
            for (int i = 0; i < schema->num_fields(); ++i) {
                auto index = pqxx::row::size_type(i);

                try {
                    if (row[index].is_null()) {
                        auto status = builders[i]->AppendNull();
                        if (!status.ok()) {
                            throw std::runtime_error("Error appending null value: " + status.ToString());
                        }
                        continue;
                    }

                    if (string_builders[i]) {
                        auto status = string_builders[i]->Append(row[index].c_str());
                        if (!status.ok()) {
                            throw std::runtime_error("Error appending string value: " + status.ToString());
                        }
                    } else if (double_builders[i]) {
                        auto status = double_builders[i]->Append(row[index].as<double>());
                        if (!status.ok()) {
                            throw std::runtime_error("Error appending double value: " + status.ToString());
                        }
                    }
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                        "Error processing field " + std::to_string(i) + 
                        " in row " + std::to_string(row_count) + ": " + std::string(e.what())
                    );
                }
            }
        }

        // Finalize Arrow arrays
        std::vector<std::shared_ptr<arrow::Array>> arrays;
        for (size_t i = 0; i < builders.size(); ++i) {
            std::shared_ptr<arrow::Array> array;
            auto status = builders[i]->Finish(&array);
            if (!status.ok()) {
                throw std::runtime_error(
                    "Error finalizing Arrow array for column " + std::to_string(i) + 
                    ": " + status.ToString()
                );
            }
            arrays.push_back(array);
        }

        txn.commit();
        return arrow::Table::Make(schema, arrays);
        
    } catch (const std::exception& e) {
        throw std::runtime_error("Error fetching data as Arrow Table: " + std::string(e.what()));
    }
}

// Insert data into a table
void DatabaseClient::insertData(const std::string& schema, const std::string& table, const std::vector<std::map<std::string, std::string>>& rows) {
    if (!db_connection || !db_connection->is_open()) {
        throw std::runtime_error("Database connection is not open.");
    }

    try {
        pqxx::work txn(*db_connection);

        for (const auto& row : rows) {
            std::string query = "INSERT INTO " + schema + "." + table + " (";
            std::string values = "VALUES (";
            for (auto it = row.begin(); it != row.end(); ++it) {
                query += it->first;
                values += txn.quote(it->second);
                if (std::next(it) != row.end()) {
                    query += ", ";
                    values += ", ";
                }
            }
            query += ") " + values + ")";
            txn.exec(query);
        }

        txn.commit();
        std::cout << "Data inserted successfully into " << schema << "." << table << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Error inserting data: ") + e.what());
    }
}

// Update data in a table
void DatabaseClient::updateData(const std::string& schema, const std::string& table, const std::map<std::string, std::string>& conditions, const std::map<std::string, std::string>& updates) {
    if (!db_connection || !db_connection->is_open()) {
        throw std::runtime_error("Database connection is not open.");
    }

    try {
        pqxx::work txn(*db_connection);
        std::string query = "UPDATE " + schema + "." + table + " SET ";

        for (auto it = updates.begin(); it != updates.end(); ++it) {
            query += it->first + " = " + txn.quote(it->second);
            if (std::next(it) != updates.end()) {
                query += ", ";
            }
        }

        if (!conditions.empty()) {
            query += " WHERE " + buildWhereClause(conditions);
        }

        txn.exec(query);
        txn.commit();
        std::cout << "Data updated successfully in " << schema << "." << table << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Error updating data: ") + e.what());
    }
}

// Delete data from a table
void DatabaseClient::deleteData(const std::string& schema, const std::string& table, const std::map<std::string, std::string>& conditions) {
    if (!db_connection || !db_connection->is_open()) {
        throw std::runtime_error("Database connection is not open.");
    }

    try {
        pqxx::work txn(*db_connection);
        std::string query = "DELETE FROM " + schema + "." + table;

        if (!conditions.empty()) {
            query += " WHERE " + buildWhereClause(conditions);
        }

        txn.exec(query);
        txn.commit();
        std::cout << "Data deleted successfully from " << schema << "." << table << std::endl;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Error deleting data: ") + e.what());
    }
}

// Retrieve all schemas in the database
std::vector<std::string> DatabaseClient::getSchemas() {
    std::vector<std::string> schemas;

    try {
        pqxx::work txn(*db_connection);
        
        std::string query = 
            "SELECT schema_name "
            "FROM information_schema.schemata "
            "WHERE schema_name NOT LIKE 'pg_%' "
            "AND schema_name NOT LIKE '_timescaledb%' "
            "AND schema_name NOT LIKE 'timescaledb%' "
            "AND schema_name NOT IN ('information_schema', 'admin', 'public') "
            "ORDER BY schema_name;";
        
        pqxx::result res = txn.exec(query);

        schemas.reserve(res.size());
        
        for (size_t i = 0; i < res.size(); ++i) {
            const auto& row = res[i];
            if (!row[0].is_null()) {
            std::string schema_name = row[0].c_str();
            if (std::find(schemas.begin(), schemas.end(), schema_name) == schemas.end()) {
                schemas.push_back(std::move(schema_name));
            }
            }
        }

        txn.commit();
        return schemas;
        
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Error retrieving database schemas: " + std::string(e.what())
        );
    }
}

// Retrieve all tables in a specific schema
std::vector<std::string> DatabaseClient::getTablesInSchema(const std::string& schema) {
    if (schema.empty()) {
        throw std::invalid_argument("Schema name cannot be empty");
    }

    std::vector<std::string> tables;

    try {
        pqxx::work txn(*db_connection);
        
        std::string query = 
            "SELECT table_name "
            "FROM information_schema.tables "
            "WHERE table_schema = " + txn.quote(schema) + " "
            "AND table_type = 'BASE TABLE' "
            "ORDER BY table_name;";
        
        pqxx::result res = txn.exec(query);

        tables.reserve(res.size());
        
        for (size_t i = 0; i < res.size(); ++i) {
            const auto& row = res[i];
            if (!row[0].is_null()) {
            std::string table_name = row[0].c_str();
            tables.push_back(std::move(table_name));
            }
        }

        txn.commit();
        return tables;
        
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Error retrieving tables for schema '" + schema + "': " + 
            std::string(e.what())
        );
    }
}

// Retrieve column names and types for a table
std::map<std::string, std::string> DatabaseClient::getColumnsInTable(
    const std::string& schema, const std::string& table
) {
    if (schema.empty()) {
        throw std::invalid_argument("Schema name cannot be empty");
    }
    if (table.empty()) {
        throw std::invalid_argument("Table name cannot be empty");
    }

    std::map<std::string, std::string> columns;

    try {
        pqxx::work txn(*db_connection);
        
        std::string query = 
            "SELECT "
            "    column_name, "
            "    data_type, "
            "    is_nullable, "
            "    column_default, "
            "    character_maximum_length "
            "FROM information_schema.columns "
            "WHERE table_schema = " + txn.quote(schema) + " "
            "AND table_name = " + txn.quote(table) + " "
            "ORDER BY ordinal_position;";
        
        pqxx::result res = txn.exec(query);

        for (size_t i = 0; i < res.size(); ++i) {
            const auto& row = res[i];
            if (!row["column_name"].is_null() && !row["data_type"].is_null()) {
            std::string column_name = row["column_name"].c_str();
            std::string data_type = row["data_type"].c_str();
            
            // Add length for character types
            if (!row["character_maximum_length"].is_null() && 
                (data_type == "character varying" || data_type == "character")) {
                data_type += "(" + std::string(row["character_maximum_length"].c_str()) + ")";
            }
            
            // Add nullability
            if (!row["is_nullable"].is_null()) {
                std::string nullable = row["is_nullable"].c_str();
                if (nullable == "NO") {
                data_type += " NOT NULL";
                }
            }
            
            // Add default value if exists
            if (!row["column_default"].is_null()) {
                data_type += " DEFAULT " + std::string(row["column_default"].c_str());
            }
            columns[std::move(column_name)] = std::move(data_type);
            }
        }

        txn.commit();
        return columns;
        
    } catch (const std::exception& e) {
        throw std::runtime_error(
            "Error retrieving columns for table '" + schema + "." + table + 
            "': " + std::string(e.what())
        );
    }
}

// Helper function to build WHERE clause
std::string DatabaseClient::buildWhereClause(const std::map<std::string, std::string>& conditions) {
    std::string where_clause;

    for (auto it = conditions.begin(); it != conditions.end(); ++it) {
        where_clause += it->first + " = '" + it->second + "'";
        if (std::next(it) != conditions.end()) {
            where_clause += " AND ";
        }
    }

    return where_clause;
}
