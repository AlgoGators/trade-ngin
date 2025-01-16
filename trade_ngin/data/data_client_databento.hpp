#pragma once
#include "data_client.hpp"
#include "database_interface.hpp"

class DatabentoClient : public DataClient {
public:
    DatabentoClient(const std::string& api_key) 
        : api_key_(api_key) {
        // Initialize database connection
        db_ = std::make_unique<DatabaseInterface>();
    }

    DataFrame get_contract_data(Dataset dataset, 
                              const std::string& symbol,
                              Agg agg_level,
                              RollType roll_type,
                              ContractType contract_type) override {
        // First try to get data from database
        auto table = db_->getOHLCVArrowTable(
            "2020-01-01",
            "2024-01-01",
            {symbol}
        );
        
        if (table && !table->num_rows() == 0) {
            return convertArrowToDataFrame(table);
        }
        
        // If not in database, fetch from Databento and store
        auto data = fetchFromDatabento(dataset, symbol, agg_level, roll_type, contract_type);
        storeInDatabase(data);
        return data;
    }

private:
    std::string api_key_;
    
    DataFrame fetchFromDatabento(Dataset dataset, 
                               const std::string& symbol,
                               Agg agg_level,
                               RollType roll_type,
                               ContractType contract_type);
                               
    void storeInDatabase(const DataFrame& data);
};
