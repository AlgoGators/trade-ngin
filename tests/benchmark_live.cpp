#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/postgres_database.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/portfolio/portfolio_manager.hpp"
#include "trade_ngin/risk/risk_manager.hpp"

using namespace trade_ngin;

// Simple RAII timer for benchmarking
class ScopedTimer {
public:
    ScopedTimer(const std::string& name)
        : name_(name), start_(std::chrono::high_resolution_clock::now()) {}
    ~ScopedTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start_).count();
        std::cout << "[BENCHMARK] " << name_ << ": " << duration / 1000.0 << " ms" << std::endl;
    }

private:
    std::string name_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

int main() {
    std::cout << "==================================================" << std::endl;
    std::cout << "   Trade-NGIN Live Benchmark (Postgres-Backed)    " << std::endl;
    std::cout << "==================================================" << std::endl;

    // 1. Connection Setup
    const char* env_conn = std::getenv("TRADENGIN_DB_CONN");
    std::string connection_string =
        env_conn ? env_conn : "postgresql://postgres:algogators@13.58.153.216:5432/new_algo_data";

    std::cout << "[INFO] connecting to database: " << connection_string << std::endl;

    PostgresDatabase db(connection_string);
    auto conn_res = db.connect();

    if (conn_res.is_error()) {
        std::cerr << "[CRITICAL] Failed to connect to database!" << std::endl;
        std::cerr << "Error: " << conn_res.error()->what() << std::endl;
        std::cerr << "Please ensure Postgres is running and TRADENGIN_DB_CONN is correct."
                  << std::endl;
        return 1;  // Strict failure
    }
    std::cout << "[SUCCESS] Connected to database." << std::endl;

    // 2. Data Loading - Using hardcoded symbols since database schema differs
    // The benchmark focuses on Optimizer and RiskManager performance, not data loading
    std::cout << "[INFO] Using hardcoded symbols for benchmark (database schema differs)..."
              << std::endl;
    std::vector<std::string> symbols = {
        "ES",  "NQ",  "CL",  "GC",  "ZB",  "ZN", "ZS", "ZC", "ZW",  "HG",  "SI",  "NG", "RB",
        "HO",  "6E",  "6J",  "6B",  "6A",  "6C", "6S", "YM", "RTY", "EMD", "NKD", "DX", "BTC",
        "ETH", "VX",  "ZT",  "ZF",  "KC",  "SB", "CC", "CT", "OJ",  "LBS", "LE",  "HE", "GF",
        "M2K", "MES", "MNQ", "MCL", "MGC", "PL", "PA", "TN", "TWE", "UB",  "ZQ"};

    std::cout << "[INFO] Selected " << symbols.size() << " symbols for benchmarking." << std::endl;

    // Skip actual market data fetch - we use synthetic data for the benchmark
    std::cout << "[INFO] Skipping market data fetch (using synthetic data for optimizer/risk "
                 "benchmarks)..."
              << std::endl;

    // 3. Optimization Benchmark
    std::cout << "\n--- Starting Dynamic Optimizer Benchmark ---" << std::endl;

    // Setup Dummy Portfolio State
    DynamicOptConfig opt_config;
    opt_config.max_iterations = 50;
    opt_config.cost_penalty_scalar = 50.0;
    DynamicOptimizer optimizer(opt_config);

    std::vector<double> current_pos(symbols.size(), 0.0);   // Start flat
    std::vector<double> target_pos(symbols.size(), 100.0);  // Want to buy 100 of everything
    std::vector<double> costs(symbols.size(), 0.001);       // 10bps cost
    std::vector<double> weights(symbols.size(), 1.0);       // 1 contract = 1 unit

    // Create a dummy covariance matrix (identity for simplicity)
    std::cout << "[INFO] Generating NxN covariance matrix (" << symbols.size() << "x"
              << symbols.size() << ")..." << std::endl;
    std::vector<std::vector<double>> covariance(symbols.size(),
                                                std::vector<double>(symbols.size(), 0.0));
    for (size_t i = 0; i < symbols.size(); ++i)
        covariance[i][i] = 0.0001;  // Diagonal variance

    {
        ScopedTimer timer("DynamicOptimizer::optimize()");
        auto opt_res = optimizer.optimize(current_pos, target_pos, costs, weights, covariance);
        if (opt_res.is_error()) {
            std::cerr << "[ERROR] Optimization failed: " << opt_res.error()->what() << std::endl;
        } else {
            auto res = opt_res.value();
            std::cout << "[RESULT] Dynamic Optimizer Mathematical Results:" << std::endl;
            std::cout << "  Iterations: " << res.iterations << std::endl;
            std::cout << "  Converged: " << (res.converged ? "Yes" : "No") << std::endl;
            std::cout << "  Tracking Error: " << res.tracking_error << std::endl;
            std::cout << "  Cost Penalty: " << res.cost_penalty << std::endl;

            double sum_weights = 0.0;
            std::cout << "  Top 5 Non-Zero Weights:" << std::endl;
            int count = 0;
            for (size_t i = 0; i < res.positions.size(); ++i) {
                sum_weights += res.positions[i];
                if (std::abs(res.positions[i]) > 0.0001 && count < 5) {
                    std::cout << "    " << symbols[i] << ": " << res.positions[i] << std::endl;
                    count++;
                }
            }
            std::cout << "  Sum of Weights: " << sum_weights << std::endl;
        }
    }

    // 4. Risk Manager Benchmark
    std::cout << "\n--- Starting Risk Manager Benchmark ---" << std::endl;
    RiskConfig risk_config;
    risk_config.capital = 1000000.0;
    RiskManager risk_manager(risk_config);

    // Prepare MarketData for risk manager
    MarketData risk_md;
    risk_md.ordered_symbols = symbols;
    // We need 'returns' history.
    std::cout << "[INFO] Generating history for Risk Manager (252 days x " << symbols.size()
              << " assets)..." << std::endl;
    risk_md.returns.resize(252, std::vector<double>(symbols.size(), 0.001));
    risk_md.covariance = covariance;  // Reuse matrix
    for (size_t i = 0; i < symbols.size(); ++i)
        risk_md.symbol_indices[symbols[i]] = i;

    // Create positions map
    std::unordered_map<std::string, Position> risk_positions;
    for (const auto& sym : symbols) {
        Position p;
        p.symbol = sym;
        p.quantity = Decimal(10.0);
        p.average_price = Decimal(100.0);
        risk_positions[sym] = p;
    }

    // Needed for risk calc
    std::unordered_map<std::string, double> current_prices;
    for (const auto& sym : symbols)
        current_prices[sym] = 100.0;

    {
        ScopedTimer timer("RiskManager::process_positions()");
        auto risk_res = risk_manager.process_positions(risk_positions, risk_md, current_prices);
        if (risk_res.is_error()) {
            std::cerr << "[ERROR] Risk calculation failed: " << risk_res.error()->what()
                      << std::endl;
        } else {
            auto res = risk_res.value();
            std::cout << "[RESULT] Risk Manager Mathematical Results:" << std::endl;
            std::cout << "  Recommended Scale: " << res.recommended_scale << std::endl;
            std::cout << "  Risk Exceeded: " << (res.risk_exceeded ? "Yes" : "No") << std::endl;
            std::cout << "  Metrics:" << std::endl;
            std::cout << "    Portfolio VaR: " << res.portfolio_var << std::endl;
            std::cout << "    Jump Risk: " << res.jump_risk << std::endl;
            std::cout << "    Correlation Risk: " << res.correlation_risk << std::endl;
            std::cout << "    Gross Leverage: " << res.gross_leverage << std::endl;
            std::cout << "    Net Leverage: " << res.net_leverage << std::endl;
            std::cout << "  Multipliers:" << std::endl;
            std::cout << "    Portfolio: " << res.portfolio_multiplier << std::endl;
            std::cout << "    Jump: " << res.jump_multiplier << std::endl;
            std::cout << "    Correlation: " << res.correlation_multiplier << std::endl;
            std::cout << "    Leverage: " << res.leverage_multiplier << std::endl;
        }
    }

    std::cout << "\n[BENCHMARK COMPLETE]" << std::endl;
    return 0;
}
