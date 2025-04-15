#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <iostream>

// Minimal stubs for types
using Timestamp = long long;
template<typename T> struct Result { bool ok = true; T value{}; Result() = default; Result(T v) : ok(true), value(v) {} static Result<T> success(T v = T{}) { return Result<T>(v); } static Result<T> failure() { Result<T> r; r.ok = false; return r; } };
template<> struct Result<void> { bool ok = true; Result() = default; static Result<void> success() { return Result<void>(); } static Result<void> failure() { Result<void> r; r.ok = false; return r; } };
enum class MarketDataEventType { TRADE, QUOTE, BAR, POSITION_UPDATE, SIGNAL_UPDATE, RISK_UPDATE, ORDER_UPDATE };
struct MarketDataEvent {
    MarketDataEventType type;
    std::string symbol;
    Timestamp timestamp;
    std::unordered_map<std::string, double> numeric_fields;
    std::unordered_map<std::string, std::string> string_fields;
};
using MarketDataCallback = std::function<void(const MarketDataEvent&)>;
struct SubscriberInfo {
    std::string id;
    std::vector<MarketDataEventType> event_types;
    std::vector<std::string> symbols;
    MarketDataCallback callback;
};

class MarketDataBusStub {
public:
    Result<void> subscribe(const SubscriberInfo&) { return Result<void>::success(); }
    Result<void> unsubscribe(const std::string&) { return Result<void>::success(); }
    void publish(const MarketDataEvent&) { /* no-op for stub */ }
    static MarketDataBusStub& instance() {
        static MarketDataBusStub instance;
        return instance;
    }
private:
    MarketDataBusStub() { std::cout << "MarketDataBusStub constructed\n"; }
    ~MarketDataBusStub() { std::cout << "MarketDataBusStub destructed\n"; }
    std::mutex mutex_;
}; 