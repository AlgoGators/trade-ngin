// contract.cpp
#include "contract.hpp"
#include <iostream>
#include <stdexcept>
#include <fstream>

Contract::Contract(const std::string& instrument,
                   Dataset dataset,
                   Agg schema,
                   Catalog catalog)
    : instrument_(instrument),
      dataset_(dataset),
      schema_(schema),
      catalog_(catalog)
{}

void Contract::construct(DataClient& client, RollType roll_type, ContractType contract_type) {
    std::string roll_str = to_string(roll_type);
    std::string ct_str = to_string(contract_type);
    std::string schema_str = to_string(schema_);

    std::filesystem::path data_path = std::filesystem::path(to_string(catalog_))
                                      / instrument_
                                      / schema_str
                                      / (roll_str + "-" + ct_str + "-data.parquet");
    std::filesystem::path def_path = std::filesystem::path(to_string(catalog_))
                                      / instrument_
                                      / schema_str
                                      / (roll_str + "-" + ct_str + "-definitions.parquet");

    auto range = client.get_dataset_range(dataset_);
    if (!range.has_value()) {
        throw std::runtime_error("No dataset range available.");
    }

    auto start = range->start - std::chrono::hours(24);
    auto end = range->end - std::chrono::hours(24);

    bool cache_loaded = load_from_cache(data_path, def_path);

    if(!cache_loaded) {
        std::cout << "Data and Definitions not present for " << instrument_ << "\n"
                  << "Attempting to retrieve data and definitions...\n";

        std::string symbol = instrument_ + "." + roll_str + "." + ct_str;
        auto new_data = client.get_contract_data(dataset_, symbol, schema_, roll_type, contract_type, start, end);
        if(!new_data) {
            throw std::runtime_error("Failed to fetch contract data.");
        }
        data_ = new_data;

        auto new_defs = client.get_definitions(dataset_, *data_);
        if(!new_defs) {
            throw std::runtime_error("Failed to fetch contract definitions.");
        }
        definitions_ = new_defs;

        save_to_cache(data_path, def_path);
    }

    set_attributes();
}

bool Contract::load_from_cache(const std::filesystem::path& data_path,
                               const std::filesystem::path& def_path) {
    if(std::filesystem::exists(data_path) && std::filesystem::exists(def_path)) {
        data_.emplace();
        definitions_.emplace();
        return true;
    }
    return false;
}

void Contract::save_to_cache(const std::filesystem::path& data_path,
                             const std::filesystem::path& def_path) {
    std::filesystem::create_directories(data_path.parent_path());
    {
        std::ofstream dfile(data_path.string());
        dfile << "mock data";
    }
    {
        std::ofstream ffile(def_path.string());
        ffile << "mock definitions";
    }
}

void Contract::set_attributes() {
    if(data_->empty()) {
        throw std::runtime_error("Data is empty");
    }

    size_t n = 100; 
    timestamps_.resize(n, std::chrono::system_clock::now());
    open_.resize(n, 100.0);
    high_.resize(n, 101.0);
    low_.resize(n, 99.0);
    close_.resize(n, 100.5);
    volume_.resize(n, 1000);
    instrument_ids_.resize(n, 1.0);

    set_expiration();
    perform_backadjustment();
}

void Contract::set_expiration() {
    size_t n = timestamps_.size();
    expiration_.resize(n, std::chrono::system_clock::now() + std::chrono::hours(24*30));
}

void Contract::perform_backadjustment() {
    backadjusted_ = close_;
}
