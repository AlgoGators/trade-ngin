#pragma once
#include "data_client.hpp"
#include "database_interface.hpp"
#include <memory>
#include <optional>

/**
 * @class DatabaseDataClient
 * @brief An implementation of DataClient that pulls data from a PostgreSQL source
 *        via DatabaseInterface, returning it in a DataFrame.
 */
class DatabaseDataClient : public DataClient {
public:
    /**
     * @brief Construct the client, also constructs a DatabaseInterface internally.
     */
    DatabaseDataClient();

    /**
     * @brief Attempt to find earliest and latest times from the DB for the given dataset.
     *        You can adapt the logic or store multiple datasets in different tables.
     */
    std::optional<DatasetRange> get_dataset_range(Dataset ds) override;

    std::optional<DataFrame> get_contract_data(
        Dataset ds,
        const std::string& symbol,
        Agg agg,
        RollType roll_type,
        ContractType contract_type,
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) override;

    std::optional<DataFrame> get_definitions(Dataset ds, const DataFrame& data) override;

private:
    std::unique_ptr<DatabaseInterface> db_;
}; 