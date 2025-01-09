#pragma once
#include <memory>
#include "database_interface.hpp"
#include "dataframe.hpp"
#include "contract.hpp"

class DataClient {
public:
    DataClient() : db_(std::make_unique<DatabaseInterface>()) {}
    virtual ~DataClient() = default;

    // Core data fetching methods
    virtual DataFrame get_contract_data(Dataset dataset, 
                                      const std::string& symbol,
                                      Agg agg_level,
                                      RollType roll_type,
                                      ContractType contract_type) {
        auto table = db_->getOHLCVArrowTable(
            "2020-01-01",  // You might want to make these configurable
            "2024-01-01",
            {symbol}
        );
        return convertArrowToDataFrame(table);
    }
    
    virtual MarketData get_latest_tick(const std::string& symbol) {
        auto table = db_->getLatestDataAsArrowTable(symbol);
        return convertArrowToMarketData(table);
    }
    
    virtual double get_average_volume(const std::string& symbol) {
        // Implement volume calculation from database
        return 0.0;
    }

protected:
    std::unique_ptr<DatabaseInterface> db_;
    
    // Helper methods to convert between Arrow and our DataFrame
    DataFrame convertArrowToDataFrame(std::shared_ptr<arrow::Table> table);
    MarketData convertArrowToMarketData(std::shared_ptr<arrow::Table> table);
}; 