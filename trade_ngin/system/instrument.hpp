#pragma once
#include <memory>
#include <vector>
#include <unordered_map>
#include "dataframe.hpp"
#include "data_client.hpp"
#include "signals.hpp"

// Forward declarations
class MarketDataHandler;
class SignalProcessor;
class RiskCalculator;

class Instrument {
public:
    Instrument(std::string symbol, Dataset dataset, Asset asset, double multiplier)
        : symbol_(std::move(symbol)), dataset_(dataset), 
          asset_(asset), multiplier_(multiplier) {}

    virtual ~Instrument() = default;

    // Core interface
    virtual void update(DataClient& client) = 0;
    virtual void addDataHandler(std::shared_ptr<MarketDataHandler> handler);
    virtual void addSignalProcessor(std::shared_ptr<SignalProcessor> processor);
    virtual void addRiskCalculator(std::shared_ptr<RiskCalculator> calculator);
    
    // Data access
    virtual const DataFrame& getMarketData() const { return market_data_; }
    virtual const DataFrame& getDerivedData() const { return derived_data_; }
    virtual const std::vector<double>& getSignals() const { return signals_; }
    virtual double getCurrentRisk() const { return current_risk_; }

    // Getters
    const std::string& symbol() const { return symbol_; }
    Dataset getDataset() const { return dataset_; }
    Asset getAsset() const { return asset_; }
    double multiplier() const { return multiplier_; }

protected:
    std::string symbol_;
    Dataset dataset_;
    Asset asset_;
    double multiplier_;
    
    DataFrame market_data_;
    DataFrame derived_data_;
    std::vector<double> signals_;
    double current_risk_ = 0.0;
    
    std::vector<std::shared_ptr<MarketDataHandler>> data_handlers_;
    std::vector<std::shared_ptr<SignalProcessor>> signal_processors_;
    std::vector<std::shared_ptr<RiskCalculator>> risk_calculators_;
};

class Future : public Instrument {
public:
    Future(std::string symbol, Dataset dataset, double multiplier)
        : Instrument(std::move(symbol), dataset, Asset::FUT, multiplier) {}

    void update(DataClient& client) override {
        auto data = client.get_contract_data(dataset_, symbol_, Agg::DAILY, 
                                           RollType::CALENDAR, ContractType::FRONT);
        if (!data) return;
        
        market_data_ = *data;
        
        for (auto& handler : data_handlers_) {
            derived_data_ = handler->process(market_data_);
        }
        
        for (auto& processor : signal_processors_) {
            auto new_signals = processor->processSignals(market_data_.get_column("close"));
            signals_.insert(signals_.end(), new_signals.begin(), new_signals.end());
        }
        
        for (auto& calculator : risk_calculators_) {
            current_risk_ = calculator->calculate(market_data_, derived_data_);
        }
    }
};

class Option : public Instrument {
public:
    Option(std::string symbol, Dataset dataset, double multiplier, 
           double strike, std::chrono::system_clock::time_point expiry)
        : Instrument(std::move(symbol), dataset, Asset::OPT, multiplier),
          strike_(strike), expiry_(expiry) {}

    void update(DataClient& client) override {
        // Similar to Future but with option-specific data handling
    }

private:
    double strike_;
    std::chrono::system_clock::time_point expiry_;
};

// Example market data handler
class VolatilityHandler : public MarketDataHandler {
public:
    DataFrame process(const DataFrame& market_data) override {
        // Calculate historical volatility, implied volatility, etc.
        return derived_data;
    }
};

// Example risk calculator
class OptionGreeksCalculator : public RiskCalculator {
public:
    double calculate(const DataFrame& market_data, const DataFrame& derived_data) override {
        // Calculate option Greeks and return risk metric
        return risk_value;
    }
};
