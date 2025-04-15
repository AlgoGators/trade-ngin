#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <iostream>

// Minimal stubs for types
struct StrategyConfig {};
struct StrategyMetadata { std::string id = "stub_id"; };
struct StrategyMetrics {};
enum class StrategyState { INIT, RUNNING, STOPPED };
struct Position {};
struct RiskLimits {};
struct ExecutionReport {};
struct Bar {};

template<typename T>
struct Result {
    bool ok = true;
    T value{};
    Result() = default;
    Result(T v) : ok(true), value(v) {}
    static Result<T> success(T v = T{}) { return Result<T>(v); }
    static Result<T> failure() { Result<T> r; r.ok = false; return r; }
};

// Specialization for void
template<>
struct Result<void> {
    bool ok = true;
    Result() = default;
    static Result<void> success() { return Result<void>(); }
    static Result<void> failure() { Result<void> r; r.ok = false; return r; }
};

class BaseStrategyStub {
public:
    BaseStrategyStub(std::string id, StrategyConfig config)
        : id_(std::move(id)), config_(config), state_(StrategyState::INIT) {
        std::cout << "BaseStrategyStub constructed\n";
    }
    virtual ~BaseStrategyStub() { std::cout << "BaseStrategyStub destructed\n"; }

    Result<void> initialize() { state_ = StrategyState::INIT; return Result<void>::success(); }
    Result<void> start() { state_ = StrategyState::RUNNING; return Result<void>::success(); }
    Result<void> stop() { state_ = StrategyState::STOPPED; return Result<void>::success(); }
    Result<void> pause() { return Result<void>::success(); }
    Result<void> resume() { return Result<void>::success(); }
    Result<void> on_data(const std::vector<Bar>&) { return Result<void>::success(); }
    Result<void> on_execution(const ExecutionReport&) { return Result<void>::success(); }
    Result<void> on_signal(const std::string& symbol, double signal) {
        std::lock_guard<std::mutex> lock(mtx_);
        last_signals_[symbol] = signal;
        return Result<void>::success();
    }
    StrategyState get_state() const { return state_; }
    const StrategyMetrics& get_metrics() const { return metrics_; }
    const StrategyConfig& get_config() const { return config_; }
    const StrategyMetadata& get_metadata() const { return metadata_; }
    const std::unordered_map<std::string, Position>& get_positions() const { return positions_; }
    Result<void> update_position(const std::string& symbol, const Position&) {
        std::lock_guard<std::mutex> lock(mtx_);
        positions_[symbol] = Position{};
        return Result<void>::success();
    }
    Result<void> update_risk_limits(const RiskLimits&) { return Result<void>::success(); }
    Result<void> check_risk_limits() { return Result<void>::success(); }
    Result<void> update_metrics() { return Result<void>::success(); }
    Result<void> transition_state(StrategyState new_state) { state_ = new_state; return Result<void>::success(); }
private:
    std::string id_;
    StrategyConfig config_;
    StrategyMetadata metadata_;
    StrategyMetrics metrics_;
    std::atomic<StrategyState> state_;
    std::unordered_map<std::string, Position> positions_;
    std::unordered_map<std::string, double> last_signals_;
    RiskLimits risk_limits_;
    mutable std::mutex mtx_;
}; 