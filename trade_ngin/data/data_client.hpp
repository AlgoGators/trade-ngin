#pragma once
#include "enums.hpp"
#include "dataframe.hpp"
#include <string>
#include <optional>
#include <chrono>

// Replaced by Patrcik/Arjun
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
        const std::string& symbol_str,
        Agg schema,
        RollType roll,
        ContractType ct,
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) = 0;

    virtual std::optional<DataFrame> get_definitions(
        Dataset ds,
        const DataFrame& data  // Possibly use instrument_id columns
    ) = 0;
};
