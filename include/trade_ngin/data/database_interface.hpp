// include/trade_ngin/data/database_interface.hpp

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <variant>
#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"

namespace trade_ngin {

/**
 * @brief Database value types
 */
using DbValue = std::variant<
    std::monostate,  // null
    bool,
    int,
    int64_t,
    double,
    std::string,
    std::chrono::system_clock::time_point
>;

/**
 * @brief Result row from a database query
 */
class DbRow {
public:
    virtual ~DbRow() = default;
    
    /**
     * @brief Get a column value by index
     * @param index Column index (0-based)
     * @return Value if column exists, null otherwise
     */
    virtual DbValue get(size_t index) const = 0;
    
    /**
     * @brief Get a column value by name
     * @param name Column name
     * @return Value if column exists, null otherwise
     */
    virtual DbValue get(const std::string& name) const = 0;
    
    /**
     * @brief Check if a column exists
     * @param name Column name
     * @return true if column exists, false otherwise
     */
    virtual bool has_column(const std::string& name) const = 0;
    
    /**
     * @brief Get the number of columns
     * @return Column count
     */
    virtual size_t column_count() const = 0;
    
    /**
     * @brief Get column names
     * @return Vector of column names
     */
    virtual std::vector<std::string> column_names() const = 0;
};

/**
 * @brief Result set from a database query
 */
class DbResultSet {
public:
    virtual ~DbResultSet() = default;
    
    /**
     * @brief Get row count
     * @return Number of rows in the result set
     */
    virtual size_t row_count() const = 0;
    
    /**
     * @brief Get column count
     * @return Number of columns in the result set
     */
    virtual size_t column_count() const = 0;
    
    /**
     * @brief Get a specific row
     * @param index Row index (0-based)
     * @return Row if index is valid, nullptr otherwise
     */
    virtual std::shared_ptr<DbRow> get_row(size_t index) const = 0;
    
    /**
     * @brief Get all rows
     * @return Vector of rows
     */
    virtual std::vector<std::shared_ptr<DbRow>> get_rows() const = 0;
    
    /**
     * @brief Get column names
     * @return Vector of column names
     */
    virtual std::vector<std::string> column_names() const = 0;
    
    /**
     * @brief Check if the result set is empty
     * @return true if empty, false otherwise
     */
    virtual bool is_empty() const = 0;
};

/**
 * @brief Database transaction interface
 */
class DbTransaction {
public:
    virtual ~DbTransaction() = default;
    
    /**
     * @brief Commit the transaction
     * @return Result indicating success or failure
     */
    virtual Result<void> commit() = 0;
    
    /**
     * @brief Rollback the transaction
     * @return Result indicating success or failure
     */
    virtual Result<void> rollback() = 0;
    
    /**
     * @brief Execute a query
     * @param query SQL query
     * @param params Query parameters
     * @return Result set if successful, error otherwise
     */
    virtual Result<std::shared_ptr<DbResultSet>> execute(
        const std::string& query, 
        const std::vector<DbValue>& params = {}) = 0;
};

/**
 * @brief Database connection interface
 */
class DatabaseInterface {
public:
    virtual ~DatabaseInterface() = default;
    
    /**
     * @brief Connect to the database
     * @param connection_string Connection string
     * @return Result indicating success or failure
     */
    virtual Result<void> connect(const std::string& connection_string) = 0;
    
    /**
     * @brief Disconnect from the database
     * @return Result indicating success or failure
     */
    virtual Result<void> disconnect() = 0;
    
    /**
     * @brief Check if connected to the database
     * @return true if connected, false otherwise
     */
    virtual bool is_connected() const = 0;
    
    /**
     * @brief Execute a query
     * @param query SQL query
     * @param params Query parameters
     * @return Result set if successful, error otherwise
     */
    virtual Result<std::shared_ptr<DbResultSet>> execute(
        const std::string& query, 
        const std::vector<DbValue>& params = {}) = 0;
    
    /**
     * @brief Execute a query that doesn't return a result set
     * @param query SQL query
     * @param params Query parameters
     * @return Result indicating success or failure
     */
    virtual Result<size_t> execute_non_query(
        const std::string& query, 
        const std::vector<DbValue>& params = {}) = 0;
    
    /**
     * @brief Start a transaction
     * @return Transaction if successful, error otherwise
     */
    virtual Result<std::shared_ptr<DbTransaction>> begin_transaction() = 0;
    
    /**
     * @brief Get symbols for a specific asset class
     * @param asset_class Asset class
     * @return Vector of symbols if successful, error otherwise
     */
    virtual Result<std::vector<std::string>> get_symbols(AssetClass asset_class) = 0;
    
    /**
     * @brief Get market data for a set of symbols
     * @param symbols Vector of symbols
     * @param start_date Start date
     * @param end_date End date
     * @param frequency Data frequency
     * @return Result set if successful, error otherwise
     */
    virtual Result<std::shared_ptr<DbResultSet>> get_market_data(
        const std::vector<std::string>& symbols,
        const std::chrono::system_clock::time_point& start_date,
        const std::chrono::system_clock::time_point& end_date,
        DataFrequency frequency) = 0;
};

/**
 * @brief Factory for creating database instances
 */
class DatabaseFactory {
public:
    /**
     * @brief Create a database instance
     * @param type Database type
     * @return Database instance
     */
    static std::shared_ptr<DatabaseInterface> create(const std::string& type);
};

} // namespace trade_ngin