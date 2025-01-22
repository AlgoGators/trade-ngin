#pragma once
#include <memory>
#include <chrono>
#include <string>
#include "dataframe.hpp"

struct MarketData {
    std::string timestamp;
    std::string symbol;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

class MarketDataHandler {
public:
    virtual ~MarketDataHandler() = default;
    virtual DataFrame process(const DataFrame& data) = 0;
};

class TickDataHandler : public MarketDataHandler {
public:
    DataFrame process(const DataFrame& data) override;
    void setTickThreshold(double threshold) { tick_threshold_ = threshold; }
private:
    double tick_threshold_ = 0.0001;
};

class MarketMicrostructureHandler : public MarketDataHandler {
public:
    DataFrame process(const DataFrame& data) override;
    void setOrderBookDepth(int depth) { book_depth_ = depth; }
private:
    int book_depth_ = 10;
}; 