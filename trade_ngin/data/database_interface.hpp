#ifndef DATABASE_INTERFACE_HPP
#define DATABASE_INTERFACE_HPP

#include <string>
#include <memory>
#include <pqxx/pqxx>
#include <arrow/api.h>
#include <vector>

class DatabaseInterface {
public:
    explicit DatabaseInterface();
    ~DatabaseInterface();

    // Methods to query the database
    std::shared_ptr<arrow::Table> getOHLCVArrowTable(
        const std::string& start_date,
        const std::string& end_date,
        const std::vector<std::string>& symbols);

private:
    std::unique_ptr<pqxx::connection> db_connection;
    std::string buildConnectionString() const;
};

#endif // DATABASE_INTERFACE_HPP
