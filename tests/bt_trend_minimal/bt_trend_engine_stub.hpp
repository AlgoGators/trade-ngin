#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <iostream>

template<typename T> struct Result { bool ok = true; T value{}; Result() = default; Result(T v) : ok(true), value(v) {} static Result<T> success(T v = T{}) { return Result<T>(v); } static Result<T> failure() { Result<T> r; r.ok = false; return r; } };
template<> struct Result<void> { bool ok = true; Result() = default; static Result<void> success() { return Result<void>(); } static Result<void> failure() { Result<void> r; r.ok = false; return r; } };
struct Bar {};
struct ExecutionReport {};
struct Position {};
struct RiskResult {};
using Timestamp = long long;
struct BacktestResults {
    double total_return{0.0};
    std::vector<ExecutionReport> executions;
    std::vector<Position> positions;
    std::vector<std::pair<Timestamp, double>> equity_curve;
};
struct BacktestConfig { std::string version{"1.0.0"}; };
struct StrategyInterface { virtual ~StrategyInterface() = default; };
struct PortfolioManager { virtual ~PortfolioManager() = default; };

class BtTrendEngineStub {
public:
    BtTrendEngineStub(BacktestConfig config, std::shared_ptr<void> db = nullptr) : config_(config) { std::cout << "BtTrendEngineStub constructed\n"; }
    ~BtTrendEngineStub() { std::cout << "BtTrendEngineStub destructed\n"; }
    Result<BacktestResults> run(std::shared_ptr<StrategyInterface>) { return Result<BacktestResults>::success(BacktestResults{}); }
    Result<BacktestResults> run_portfolio(std::shared_ptr<PortfolioManager>) { return Result<BacktestResults>::success(BacktestResults{}); }
    Result<void> save_results_to_db(const BacktestResults&) { return Result<void>::success(); }
    Result<void> save_results_to_csv(const BacktestResults&) { return Result<void>::success(); }
    Result<BacktestResults> load_results(const std::string&) const { return Result<BacktestResults>::success(BacktestResults{}); }
    static Result<std::unordered_map<std::string, double>> compare_results(const BacktestResults&, const BacktestResults&) { return Result<std::unordered_map<std::string, double>>::success({}); }
private:
    BacktestConfig config_;
    std::mutex mutex_;
}; 