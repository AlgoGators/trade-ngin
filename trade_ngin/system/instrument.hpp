#pragma once
#include <string>
#include <memory>
#include <vector>
#include <stdexcept>
#include "contract.hpp"

#pragma once
#include <string>
#include <vector>

// Minimal Instrument interface
class Instrument {
public:
    virtual ~Instrument() = default;
    virtual const std::string& name() const = 0;
    virtual double multiplier() const = 0;
    virtual const std::vector<double>& price() const = 0; 
};

// Enum for asset type
enum class Asset {
    FUT,
    OPT,
    EQ
};

inline std::string to_string(Asset a) {
    switch(a) {
        case Asset::FUT: return "FUT";
        case Asset::OPT: return "OPT";
        case Asset::EQ:  return "EQ";
    }
    throw std::runtime_error("Unknown Asset");
}

// Enum for instrument type
enum class InstrumentType {
    FUTURE,
    UNKNOWN
};

// Converts a string to an InstrumentType
inline InstrumentType instrument_type_from_str(const std::string& val) {
    std::string lower = val;
    for (auto &c: lower) c = ::tolower(c);
    if (lower == "future") return InstrumentType::FUTURE;
    return InstrumentType::UNKNOWN;
}

// Base Instrument class
class Instrument {
public:
    Instrument(std::string symbol, Dataset dataset, Asset asset, double multiplier)
        : symbol_(std::move(symbol)), dataset_(dataset), asset_(asset), multiplier_(multiplier) {}

    virtual ~Instrument() = default;

    const std::string& symbol() const { return symbol_; }
    Dataset get_dataset() const { return dataset_; }
    Asset get_asset() const { return asset_; }
    double multiplier() const { return multiplier_; }

    // Price getter must be overridden by derived classes.
    virtual const std::vector<double>& price() const = 0;

protected:
    std::string symbol_;
    Dataset dataset_;
    Asset asset_;
    double multiplier_;
};

// Future instrument derived class
class Future : public Instrument {
public:
    Future(std::string symbol, Dataset dataset, double multiplier)
        : Instrument(std::move(symbol), dataset, Asset::FUT, multiplier) {}

    void add_data(DataClient& client, Agg schema, RollType roll_type, ContractType contract_type, Catalog catalog = Catalog::DATABENTO) {
        Contract c(symbol_, dataset_, schema, catalog);
        c.construct(client, roll_type, contract_type);
        contracts_.push_back(std::move(c));

        if (contract_type == ContractType::FRONT) {
            front_ = &contracts_.back();
            backadjusted_price_ = front_->backadjusted();
        }
    }

    void add_norgate_data() {
        // Example if you have NORGATE data:
        // This would construct using Norgate logic, similar to Databento but with Norgate dataset.
        // For demonstration, just show how it would be done.
        Contract c(symbol_, dataset_, Agg::DAILY, Catalog::NORGATE);
        // Assuming roll_type/calendar logic for front contract in Norgate scenario:
        c.construct(*norgate_client_, RollType::CALENDAR, ContractType::FRONT);
        contracts_.push_back(std::move(c));
        front_ = &contracts_.back();
        backadjusted_price_ = front_->backadjusted();
    }

    const std::vector<double>& price() const override {
        if (backadjusted_price_.empty()) {
            throw std::runtime_error("No price data loaded for Future instrument: " + symbol_);
        }
        return backadjusted_price_;
    }

    const Contract* front() const { return front_; }

    // Optional: If you want the back contract or others, you can implement similar logic.

    // A method to initialize a Norgate client if needed
    void set_norgate_client(DataClient* client) {
        norgate_client_ = client;
    }

private:
    std::vector<Contract> contracts_;
    const Contract* front_ = nullptr;
    DataClient* norgate_client_ = nullptr; // optional if you need a separate client for Norgate
    std::vector<double> backadjusted_price_;
};
