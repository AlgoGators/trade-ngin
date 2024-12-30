class DataClient {
    // Add missing interface methods needed by Instrument
    virtual DataFrame get_contract_data(Dataset dataset, 
                                      const std::string& symbol,
                                      Agg agg_level,
                                      RollType roll_type,
                                      ContractType contract_type) = 0;
    
    virtual MarketData get_latest_tick(const std::string& symbol) = 0;
    virtual double get_average_volume(const std::string& symbol) = 0;
}; 