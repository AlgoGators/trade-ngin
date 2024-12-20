// contract.hpp
#pragma once
#include <string>
#include <vector>
#include <filesystem>
#include <chrono>
#include <optional>

// Enums
enum class Catalog {
    DATABENTO,
    NORGATE
};

inline std::string to_string(Catalog c) {
    switch (c) {
        case Catalog::DATABENTO: return "data/catalog/databento";
        case Catalog::NORGATE:   return "data/catalog/norgate";
    }
    throw std::runtime_error("Unknown Catalog");
}

enum class Dataset {
    CME
};

inline std::string to_string(Dataset d) {
    switch(d) {
        case Dataset::CME: return "GLBX.MDP3";
    }
    throw std::runtime_error("Unknown Dataset");
}

enum class Agg {
    DAILY,
    HOURLY,
    MINUTE,
    SECOND
};

inline std::string to_string(Agg a) {
    switch (a) {
        case Agg::DAILY:  return "ohlcv-1d";
        case Agg::HOURLY: return "ohlcv-1h";
        case Agg::MINUTE: return "ohlcv-1m";
        case Agg::SECOND: return "ohlcv-1s";
    }
    throw std::runtime_error("Unknown Agg");
}

enum class RollType {
    CALENDAR,
    OPEN_INTEREST,
    VOLUME
};

inline std::string to_string(RollType r) {
    switch(r) {
        case RollType::CALENDAR:      return "c";
        case RollType::OPEN_INTEREST: return "n";
        case RollType::VOLUME:        return "v";
    }
    throw std::runtime_error("Unknown RollType");
}

enum class ContractType {
    FRONT,
    BACK,
    THIRD,
    FOURTH,
    FIFTH
};

inline std::string to_string(ContractType c) {
    switch(c) {
        case ContractType::FRONT:  return "0";
        case ContractType::BACK:   return "1";
        case ContractType::THIRD:  return "2";
        case ContractType::FOURTH: return "3";
        case ContractType::FIFTH:  return "4";
    }
    throw std::runtime_error("Unknown ContractType");
}

// Simple DataFrame stub
class DataFrame {
public:
    bool empty() const { return true; } // Stub, real implementation needed
};

// DataClient interface stub
class DataClient {
public:
    struct DatasetRange {
        std::chrono::system_clock::time_point start;
        std::chrono::system_clock::time_point end;
    };

    virtual ~DataClient() = default;
    virtual std::optional<DatasetRange> get_dataset_range(Dataset) = 0;
    virtual std::optional<DataFrame> get_contract_data(
        Dataset,
        const std::string&,
        Agg,
        RollType,
        ContractType,
        std::chrono::system_clock::time_point,
        std::chrono::system_clock::time_point
    ) = 0;
    virtual std::optional<DataFrame> get_definitions(Dataset, const DataFrame&) = 0;
};

// Contract class
class Contract {
public:
    Contract(const std::string& instrument, Dataset dataset, Agg schema, Catalog catalog);

    void construct(DataClient& client, RollType roll_type, ContractType contract_type);

    const std::string& instrument() const { return instrument_; }
    Dataset get_dataset() const { return dataset_; }
    Agg get_schema() const { return schema_; }
    Catalog get_catalog() const { return catalog_; }

    const std::vector<std::chrono::system_clock::time_point>& timestamps() const { return timestamps_; }
    const std::vector<double>& open() const { return open_; }
    const std::vector<double>& high() const { return high_; }
    const std::vector<double>& low() const { return low_; }
    const std::vector<double>& close() const { return close_; }
    const std::vector<double>& volume() const { return volume_; }
    const std::vector<double>& backadjusted() const { return backadjusted_; }
    const std::vector<double>& instrument_ids() const { return instrument_ids_; }
    const std::vector<std::chrono::system_clock::time_point>& expiration() const { return expiration_; }

private:
    bool load_from_cache(const std::filesystem::path& data_path,
                         const std::filesystem::path& def_path);

    void save_to_cache(const std::filesystem::path& data_path,
                       const std::filesystem::path& def_path);

    void set_attributes();
    void set_expiration();
    void perform_backadjustment();

    std::string instrument_;
    Dataset dataset_;
    Agg schema_;
    Catalog catalog_;

    std::optional<DataFrame> data_;
    std::optional<DataFrame> definitions_;

    std::vector<std::chrono::system_clock::time_point> timestamps_;
    std::vector<double> open_, high_, low_, close_, volume_, instrument_ids_;
    std::vector<std::chrono::system_clock::time_point> expiration_;
    std::vector<double> backadjusted_;
};
