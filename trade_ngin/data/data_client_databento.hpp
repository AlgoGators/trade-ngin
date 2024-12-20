#pragma once
#include "data_client.hpp"
#include <iostream>

// Replaced by PAtrick code after seeing it
class DatabentoClient : public DataClient {
public:
    DatabentoClient(const std::string& api_key) : api_key_(api_key) {}

    std::optional<DatasetRange> get_dataset_range(Dataset ds) override {
        // mock:
        DatasetRange r;
        r.start = std::chrono::system_clock::now() - std::chrono::hours(24*365);
        r.end   = std::chrono::system_clock::now();
        return r;
    }

    std::optional<DataFrame> get_contract_data(
        Dataset ds,
        const std::string& symbol_str,
        Agg schema,
        RollType roll,
        ContractType ct,
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end
    ) override {
        DataFrame df;
        return df;
    }

    std::optional<DataFrame> get_definitions(
        Dataset ds,
        const DataFrame& data
    ) override {
        DataFrame defs;
        return defs;
    }

private:
    std::string api_key_;
};
