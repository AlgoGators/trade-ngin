#pragma once
#include "../data/dataframe.hpp"

class MarketDataHandler {
public:
    virtual ~MarketDataHandler() = default;
    virtual DataFrame process(const DataFrame& market_data) = 0;
};

class SignalProcessor {
public:
    virtual ~SignalProcessor() = default;
    virtual std::vector<double> processSignals(const std::vector<double>& data) = 0;
};

class RiskCalculator {
public:
    virtual ~RiskCalculator() = default;
    virtual double calculate(const DataFrame& market_data, const DataFrame& derived_data) = 0;
}; 