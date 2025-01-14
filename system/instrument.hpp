#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include "../data/dataframe.hpp"
#include "../data/enums.hpp"
#include "data_client.hpp"
#include "signals.hpp"

class Instrument {
public:
    Instrument(std::string symbol, Dataset dataset, Asset asset, double multiplier)
        : symbol_(std::move(symbol)), dataset_(dataset), 
          asset_(asset), multiplier_(multiplier) {}

    virtual ~Instrument() = default;

    // Getters
    const std::string& symbol() const { return symbol_; }
    Dataset getDataset() const { return dataset_; }
    Asset getAsset() const { return asset_; }
    double getMultiplier() const { return multiplier_; }

    // Data access
    virtual DataFrame getLatestTick() const { 
        return market_data_; 
    }

    virtual double getCurrentPrice() const { 
        auto prices = market_data_.get_column("close");
        return prices.empty() ? 0.0 : prices.back();
    }

    virtual double getCurrentVolatility() const { 
        auto vol = derived_data_.get_column("volatility");
        return vol.empty() ? 0.0 : vol.back();
    }

    virtual double getAverageVolume() const { 
        auto vol = derived_data_.get_column("avg_volume");
        return vol.empty() ? 0.0 : vol.back();
    }

    // Signal access
    const std::vector<double>& getSignalValues(const std::string& signal_name) const {
        auto it = signal_values_.find(signal_name);
        if (it == signal_values_.end()) {
            static const std::vector<double> empty;
            return empty;
        }
        return it->second;
    }

    // Update methods
    virtual void update(DataClient& client) = 0;
    virtual void addSignalProcessor(std::shared_ptr<signals::Signal> signal) {
        signals_.push_back(std::move(signal));
    }

protected:
    std::string symbol_;
    Dataset dataset_;
    Asset asset_;
    double multiplier_;
    DataFrame market_data_;
    DataFrame derived_data_;
    std::unordered_map<std::string, std::vector<double>> signal_values_;
    std::vector<std::shared_ptr<signals::Signal>> signals_;
};

class Future : public Instrument {
public:
    Future(std::string symbol, Dataset dataset, double multiplier)
        : Instrument(std::move(symbol), dataset, Asset::FUT, multiplier) {}

    void update(DataClient& client) override {
        auto data = client.get_contract_data(dataset_, symbol_, Agg::DAILY, 
                                           RollType::CALENDAR, ContractType::FRONT);
        market_data_ = data;

        // Process signals
        for (const auto& signal : signals_) {
            auto values = signal->calculate(market_data_);
            signal_values_[signal->name()] = values;
        }
    }
}; 