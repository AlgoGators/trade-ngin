#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <optional>
#include "../data/dataframe.hpp"
#include "../data/enums.hpp"

/**
 * @class DataClient
 * @brief Abstract interface for retrieving data from some source (DB, REST, etc.).
 *
 * This interface returns optional<DataFrame> for convenience. If data cannot be fetched,
 * it can return std::nullopt. 
 */
class DataClient {
public:
    virtual ~DataClient() = default;

    struct DatasetRange {
        std::chrono::system_clock::time_point start;
        std::chrono::system_clock::time_point end;
    };

    virtual std::optional<DatasetRange> get_dataset_range(Dataset ds) = 0;

    virtual std::optional<DataFrame> get_contract_data(
        Dataset ds,
        const std::string& symbol,
        Agg agg,
        RollType roll_type,
        ContractType contract_type,
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) = 0;

    // You could have additional methods as needed, e.g. definitions, real-time ticks, etc.
    virtual std::optional<DataFrame> get_definitions(Dataset ds, const DataFrame& data) = 0;
    
    virtual DataFrame get_contract_data(Dataset dataset, 
                                      const std::string& symbol,
                                      Agg agg_level,
                                      RollType roll_type,
                                      ContractType contract_type) = 0;
    
    virtual DataFrame get_latest_tick(const std::string& symbol) = 0;
    virtual double get_average_volume(const std::string& symbol) = 0;
}; 