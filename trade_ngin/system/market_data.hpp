#pragma once
#include <memory>
#include <chrono>
#include "dataframe.hpp"

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