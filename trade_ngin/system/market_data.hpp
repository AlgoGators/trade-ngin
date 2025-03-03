// #pragma once
// #include <memory>
// #include <chrono>
// #include <string>
// //#include "dataframe.hpp"

// namespace ibkr {

// struct MarketData {
//     std::string timestamp;
//     std::string symbol;
//     double open;
//     double high;
//     double low;
//     double close;
//     double volume;
// };

// } // namespace ibkr

// class MarketDataHandler {
// public:
//     virtual ~MarketDataHandler() = default;
//     virtual ibkr::DataFrame process(const ibkr::DataFrame& data) = 0;
// };

// class TickDataHandler : public MarketDataHandler {
// public:
//     ibkr::DataFrame process(const ibkr::DataFrame& data) override;
//     void setTickThreshold(double threshold) { tick_threshold_ = threshold; }
// private:
//     double tick_threshold_ = 0.0001;
// };

// class MarketMicrostructureHandler : public MarketDataHandler {
// public:
//     ibkr::DataFrame process(const ibkr::DataFrame& data) override;
//     void setOrderBookDepth(int depth) { book_depth_ = depth; }
// private:
//     int book_depth_ = 10;
// }; 