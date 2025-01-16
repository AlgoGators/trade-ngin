#ifndef DATABASE_CLIENT_HPP
#define DATABASE_CLIENT_HPP

#include <pqxx/pqxx>
#include <arrow/api.h>
#include <string>
#include <vector>
#include <map>
#include <memory>

class DatabaseClient {
public:
    /**
     * Constructor
     * @param connection_string The connection string to the PostgreSQL database.
     */
    explicit DatabaseClient(const std::string& connection_string);

    /**
     * Destructor
     */
    ~DatabaseClient();

    /**
     * Establish a connection to the database.
     */
    void connect();

    /**
     * Close the database connection.
     */
    void disconnect();

    /**
     * Execute a raw SQL query and return the result.
     * @param query The SQL query to execute.
     * @return A pqxx::result containing the query results.
     */
    pqxx::result executeQuery(const std::string& query);

    /**
     * Fetch query results as an Apache Arrow Table.
     * @param query The SQL query to execute.
     * @param schema The Arrow schema defining the structure of the table.
     * @return A shared pointer to the Arrow Table containing the query results.
     */
    std::shared_ptr<arrow::Table> fetchDataAsArrowTable(const std::string& query, const std::shared_ptr<arrow::Schema>& schema);

    /**
     * Insert data into a table.
     * @param schema The schema of the target table.
     * @param table The name of the target table.
     * @param rows A vector of maps representing rows to insert (column name -> value).
     */
    void insertData(const std::string& schema, const std::string& table, const std::vector<std::map<std::string, std::string>>& rows);

    /**
     * Update data in a table.
     * @param schema The schema of the target table.
     * @param table The name of the target table.
     * @param conditions A map of conditions (column name -> value) to filter rows to update.
     * @param updates A map of column updates (column name -> new value).
     */
    void updateData(const std::string& schema, const std::string& table, const std::map<std::string, std::string>& conditions, const std::map<std::string, std::string>& updates);

    /**
     * Delete data from a table.
     * @param schema The schema of the target table.
     * @param table The name of the target table.
     * @param conditions A map of conditions (column name -> value) to filter rows to delete.
     */
    void deleteData(const std::string& schema, const std::string& table, const std::map<std::string, std::string>& conditions);

    /**
     * Retrieve all schemas in the database.
     * @return A vector of schema names.
     */
    std::vector<std::string> getSchemas();

    /**
     * Retrieve all tables within a specific schema.
     * @param schema The name of the schema.
     * @return A vector of table names.
     */
    std::vector<std::string> getTablesInSchema(const std::string& schema);

    /**
     * Retrieve column names and types for a table.
     * @param schema The schema of the table.
     * @param table The name of the table.
     * @return A map where keys are column names and values are column types.
     */
    std::map<std::string, std::string> getColumnsInTable(const std::string& schema, const std::string& table);

private:
    pqxx::connection* db_connection;  // Pointer to the database connection
    std::string connection_string;    // Connection string to the database

    /**
     * Build a WHERE clause from conditions.
     * @param conditions A map of conditions (column name -> value).
     * @return A string representing the WHERE clause.
     */
    std::string buildWhereClause(const std::map<std::string, std::string>& conditions);

    /**
     * Split a large query into batches.
     * @param rows A vector of maps representing rows to process.
     * @param batch_size The size of each batch.
     * @return A vector of batches, each containing a subset of rows.
     */
    std::vector<std::vector<std::map<std::string, std::string>>> createBatches(const std::vector<std::map<std::string, std::string>>& rows, int batch_size = 1000);
};

#endif // DATABASE_CLIENT_HPP
