#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <iostream>

// Minimal stubs for types
struct Bar {};
struct Position {};
struct ExecutionReport {};
struct DynamicOptConfig {};
struct RiskConfig {};
struct ConfigBase { virtual ~ConfigBase() = default; };
struct PortfolioConfig : public ConfigBase {
    double total_capital{0.0};
    double reserve_capital{0.0};
    double max_strategy_allocation{1.0};
    double min_strategy_allocation{0.0};
    bool use_optimization{false};
    bool use_risk_management{false};
    DynamicOptConfig opt_config;
    RiskConfig risk_config;
    std::string version{"1.0.0"};
    PortfolioConfig() = default;
    PortfolioConfig(double t, double r, double maxa, double mina, bool opt, bool risk)
        : total_capital(t), reserve_capital(r), max_strategy_allocation(maxa), min_strategy_allocation(mina), use_optimization(opt), use_risk_management(risk) {}
    std::string get_config_type() const { return "PortfolioConfig"; }
    std::string get_config_version() const { return version; }
    std::string to_string() const { return "{}"; }
    bool from_string(const std::string&) { return true; }
};
struct StrategyInterface { virtual ~StrategyInterface() = default; };
template<typename T> struct Result { bool ok = true; T value{}; Result() = default; Result(T v) : ok(true), value(v) {} static Result<T> success(T v = T{}) { return Result<T>(v); } static Result<T> failure() { Result<T> r; r.ok = false; return r; } };
template<> struct Result<void> { bool ok = true; Result() = default; static Result<void> success() { return Result<void>(); } static Result<void> failure() { Result<void> r; r.ok = false; return r; } };

class PortfolioManagerStub {
public:
    explicit PortfolioManagerStub(PortfolioConfig config, std::string id = "PORTFOLIO_MANAGER")
        : config_(config), id_(std::move(id)), instance_id_(id_) { std::cout << "PortfolioManagerStub constructed\n"; }
    ~PortfolioManagerStub() { std::cout << "PortfolioManagerStub destructed\n"; }
    Result<void> add_strategy(std::shared_ptr<StrategyInterface>, double, bool = false, bool = false) { return Result<void>::success(); }
    Result<void> process_market_data(const std::vector<Bar>&) { return Result<void>::success(); }
    Result<void> update_allocations(const std::unordered_map<std::string, double>&) { return Result<void>::success(); }
    std::unordered_map<std::string, Position> get_portfolio_positions() const { return {}; }
    std::unordered_map<std::string, double> get_required_changes() const { return {}; }
    std::vector<ExecutionReport> get_recent_executions() const { return {}; }
    void clear_execution_history() {}
    std::vector<std::shared_ptr<StrategyInterface>> get_strategies() const { return {}; }
    double get_portfolio_value(const std::unordered_map<std::string, double>&) const { return 0.0; }
    const PortfolioConfig& get_config() const { return config_; }
    void set_risk_manager(void*) {}
private:
    PortfolioConfig config_;
    std::string id_;
    std::mutex mutex_;
    const std::string instance_id_;
}; 