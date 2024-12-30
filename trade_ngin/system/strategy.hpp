#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include "dataframe.hpp"
#include "signals.hpp"
#include "risk_engine.hpp"
#include "instrument.hpp"

class Strategy {
public:
    struct StrategyConfig {
        double capital_allocation;
        double max_leverage;
        double position_limit;
        double risk_limit;
        std::unordered_map<std::string, double> params;
    };

    Strategy(std::string name, StrategyConfig config) 
        : name_(std::move(name)), config_(std::move(config)) {}
    
    virtual ~Strategy() = default;

    // Core strategy interface
    virtual DataFrame positions() = 0;
    virtual void update(const DataFrame& market_data) = 0;
    virtual void onFill(const Order& order) {}  // Callback for order fills
    virtual void onTick(const MarketData& tick) {}  // Callback for tick data
    
    // Risk management interface
    virtual double getMaxLeverage() const { return config_.max_leverage; }
    virtual double getPositionLimit() const { return config_.position_limit; }
    virtual double getRiskLimit() const { return config_.risk_limit; }
    
    // Performance tracking
    virtual void updateMetrics(const RiskEngine::RiskMetrics& metrics) {}
    
    // Configuration
    virtual void setParam(const std::string& name, double value) {
        config_.params[name] = value;
    }
    
    // Getters
    const std::string& getName() const { return name_; }
    double getCapital() const { return config_.capital_allocation; }
    const StrategyConfig& getConfig() const { return config_; }

    // Add methods to access instrument data
    void processInstrumentUpdate(const Instrument& instrument) {
        auto market_data = instrument.getMarketData();
        auto volatility = instrument.getCurrentVolatility();
        auto signals = instrument.getSignals("trend");
        
        // Update position sizing based on volatility
        double position_size = calculatePositionSize(volatility);
        
        // Update signals
        signal_processor_.processSignals(market_data);
        
        // Update positions based on signals and risk
        updatePositions(signals, position_size);
    }

protected:
    std::string name_;
    StrategyConfig config_;
    DataFrame current_positions_;
    signals::SignalProcessor signal_processor_;

    virtual double calculatePositionSize(double volatility) {
        return config_.capital_allocation * (config_.risk_limit / volatility);
    }
};

// Trend Following Strategy Implementation
class TrendFollowingStrategy : public Strategy {
public:
    TrendFollowingStrategy(double capital, double contract_size, 
                          double risk_target = 0.2, double fx = 1.0, double idm = 2.5)
        : Strategy("TrendFollowing", {capital, 1.0, 1.0, 0.2, {{"contract_size", contract_size}, {"risk_target", risk_target}, {"fx", fx}, {"idm", idm}}}), 
          multiplier_(contract_size), risk_target_(risk_target),
          fx_(fx), idm_(idm) {}

    DataFrame positions() override {
        return current_positions_;
    }

    void update(const DataFrame& market_data) override {
        auto prices = market_data.get_column("close");
        auto vols = market_data.get_column("volatility");
        
        // Calculate signals using your existing signal code
        std::vector<double> combined_forecast = generatePositions(prices);
        
        // Convert to DataFrame format
        std::unordered_map<std::string, std::vector<double>> pos_map;
        for (const auto& col : market_data.columns()) {
            pos_map[col] = combined_forecast;
        }
        current_positions_ = DataFrame(pos_map);
    }

private:
    double multiplier_;
    double risk_target_;
    double fx_;
    double idm_;
};

// Buy and Hold Strategy Implementation
class BuyAndHoldStrategy : public Strategy {
public:
    BuyAndHoldStrategy(double capital) 
        : Strategy("BuyAndHold", {capital, 1.0, 1.0, 0.2, {}}) {}

    DataFrame positions() override {
        return current_positions_;
    }

    void update(const DataFrame& market_data) override {
        if (current_positions_.empty()) {
            // Initialize positions on first update
            std::unordered_map<std::string, std::vector<double>> pos_map;
            for (const auto& col : market_data.columns()) {
                std::vector<double> pos(market_data.rows(), 1.0);  // Always long
                pos_map[col] = pos;
            }
            current_positions_ = DataFrame(pos_map);
        }
    }
}; 