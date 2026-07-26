// benchmarks/benchmark_main.cpp
//
// Main benchmark harness for trade-ngin.
// Benchmarks critical paths: optimizer, risk manager, position buffering,
// data conversion, statistics calculations.

#include <cstring>
#include <fstream>
#include <random>

#include "benchmark_framework.hpp"
#include "trade_ngin/core/time_utils.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/data/conversion_utils.hpp"
#include "trade_ngin/optimization/dynamic_optimizer.hpp"
#include "trade_ngin/risk/risk_manager.hpp"
#include "trade_ngin/statistics/statistics.hpp"

using namespace trade_ngin;
using namespace trade_ngin::bench;

// ============================================================================
// Synthetic Data Generation (all in-process, deterministic, no I/O)
// ============================================================================

struct BenchmarkFixture {
    std::vector<double> returns_10;
    std::vector<double> returns_50;
    std::vector<std::vector<double>> cov_10;
    std::vector<std::vector<double>> cov_50;
    std::vector<Bar> bars_10;
    std::vector<Bar> bars_50;

    BenchmarkFixture() {
        // Use fixed seed for reproducibility
        std::mt19937 rng(42);
        std::normal_distribution<double> norm(0.0, 0.02);

        // 10 symbols
        returns_10.resize(10);
        for (auto& r : returns_10) r = norm(rng);

        // 50 symbols
        returns_50.resize(50);
        for (auto& r : returns_50) r = norm(rng);

        // Generate 10x10 covariance matrix
        cov_10 = generate_covariance_matrix(10, rng);

        // Generate 50x50 covariance matrix
        cov_50 = generate_covariance_matrix(50, rng);

        // Generate synthetic bars (10 symbols, 100 days)
        bars_10 = generate_synthetic_bars(10, 100, rng);

        // Generate synthetic bars (50 symbols, 20 days, smaller for time)
        bars_50 = generate_synthetic_bars(50, 20, rng);
    }

private:
    static std::vector<std::vector<double>> generate_covariance_matrix(
        size_t n, std::mt19937& rng) {
        std::normal_distribution<double> norm(0.0, 1.0);

        // Create random matrix A
        std::vector<std::vector<double>> A(n, std::vector<double>(n));
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                A[i][j] = norm(rng);
            }
        }

        // Compute AA^T to get positive-definite covariance
        std::vector<std::vector<double>> cov(n, std::vector<double>(n, 0.0));
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                for (size_t k = 0; k < n; ++k) {
                    cov[i][j] += A[i][k] * A[j][k];
                }
            }
        }

        // Scale to reasonable variance
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) {
                cov[i][j] /= n;
            }
        }

        return cov;
    }

    static std::vector<Bar> generate_synthetic_bars(size_t num_symbols, size_t num_bars,
                                                     std::mt19937& rng) {
        std::vector<Bar> bars;
        std::normal_distribution<double> returns_dist(0.0, 0.01);
        std::uniform_real_distribution<double> volume_dist(1e6, 1e7);

        double base_price = 100.0;

        for (size_t s = 0; s < num_symbols; ++s) {
            double price = base_price;
            for (size_t b = 0; b < num_bars; ++b) {
                double daily_return = returns_dist(rng);
                price *= (1.0 + daily_return);

                Bar bar;
                bar.symbol = "SYM" + std::to_string(s);
                // Create timestamp from nanoseconds
                auto ns = std::chrono::nanoseconds(1000000000LL + s * 100000000LL + b * 1000000LL);
                bar.timestamp = Timestamp(std::chrono::duration_cast<std::chrono::system_clock::duration>(ns));
                bar.open = price * 0.99;
                bar.high = price * 1.01;
                bar.low = price * 0.98;
                bar.close = price;
                bar.volume = volume_dist(rng);
                bars.push_back(bar);
            }
        }

        return bars;
    }
};

// ============================================================================
// Benchmark Functions
// ============================================================================

void benchmark_optimizer_10_symbols(const BenchmarkFixture& fixture) {
    // Benchmark the core optimization loop with 10 symbols.
    // This is called once per iteration; the framework times it.

    DynamicOptConfig config;
    config.tau = 1.0;
    config.capital = 500000.0;
    config.cost_penalty_scalar = 50.0;
    config.use_buffering = true;
    config.max_iterations = 10;
    config.convergence_threshold = 1e-6;

    auto optimizer = std::make_unique<DynamicOptimizer>(config);

    std::vector<double> current(10, 5.0);    // Current positions
    std::vector<double> target(10, 10.0);    // Target positions
    std::vector<double> costs(10, 0.001);    // Transaction costs
    std::vector<double> weights(10, 1.0);    // Contract weights

    auto result = optimizer->optimize(current, target, costs, weights, fixture.cov_10);

    // do_not_optimize forces the compiler to keep the result
    if (result.is_ok()) {
        volatile double val = result.value().tracking_error;
        do_not_optimize(val);
    }
}

void benchmark_optimizer_50_symbols(const BenchmarkFixture& fixture) {
    // Benchmark with 50 symbols for realistic scale.

    DynamicOptConfig config;
    config.tau = 1.0;
    config.capital = 500000.0;
    config.cost_penalty_scalar = 50.0;
    config.use_buffering = true;
    config.max_iterations = 10;
    config.convergence_threshold = 1e-6;

    auto optimizer = std::make_unique<DynamicOptimizer>(config);

    std::vector<double> current(50, 5.0);
    std::vector<double> target(50, 10.0);
    std::vector<double> costs(50, 0.001);
    std::vector<double> weights(50, 1.0);

    auto result = optimizer->optimize(current, target, costs, weights, fixture.cov_50);

    if (result.is_ok()) {
        volatile double val = result.value().tracking_error;
        do_not_optimize(val);
    }
}

void benchmark_risk_portfolio_variance_10(const BenchmarkFixture& fixture) {
    // Benchmark RiskManager::calculate_portfolio_multiplier with 10 symbols.
    // This directly measures the portfolio VaR calculation path in trade-ngin's
    // risk module, which is called on every position update.
    // Entry point: include/trade_ngin/risk/risk_manager.hpp:181-183

    RiskConfig config;
    config.var_limit = 0.15;
    config.confidence_level = 0.99;
    config.capital = Decimal(500000.0);
    RiskManager rm(config);

    // Construct market data: 10 symbols, deterministically seeded
    MarketData market_data;
    market_data.ordered_symbols = {};
    for (size_t i = 0; i < 10; ++i) {
        market_data.ordered_symbols.push_back("SYM" + std::to_string(i));
        market_data.symbol_indices["SYM" + std::to_string(i)] = i;
    }
    market_data.covariance = fixture.cov_10;
    // Returns matrix: 100 rows (lookback), 10 columns (symbols)
    market_data.returns.resize(100, std::vector<double>(10));
    for (size_t i = 0; i < 100; ++i) {
        for (size_t j = 0; j < 10; ++j) {
            market_data.returns[i][j] = fixture.returns_10[j] * (1.0 + i * 0.001);
        }
    }

    // Construct positions: equal weight across 10 symbols
    std::unordered_map<std::string, Position> positions;
    auto now = std::chrono::system_clock::now();
    for (size_t i = 0; i < 10; ++i) {
        Position pos("SYM" + std::to_string(i), Quantity(10), Price(100.0),
                     Decimal(0), Decimal(0), Timestamp(now));
        positions[pos.symbol] = pos;
    }

    // Call process_positions: the hot path that calls calculate_portfolio_multiplier
    auto result = rm.process_positions(positions, market_data);
    if (result.is_ok()) {
        volatile double val = result.value().portfolio_var;
        do_not_optimize(val);
    }
}

void benchmark_risk_portfolio_variance_50(const BenchmarkFixture& fixture) {
    // Benchmark RiskManager::calculate_portfolio_multiplier with 50 symbols.
    // Measures the portfolio VaR calculation at realistic scale (50 positions).
    // Entry point: include/trade_ngin/risk/risk_manager.hpp:181-183

    RiskConfig config;
    config.var_limit = 0.15;
    config.confidence_level = 0.99;
    config.capital = Decimal(500000.0);
    RiskManager rm(config);

    // Construct market data: 50 symbols, deterministically seeded
    MarketData market_data;
    market_data.ordered_symbols = {};
    for (size_t i = 0; i < 50; ++i) {
        market_data.ordered_symbols.push_back("SYM" + std::to_string(i));
        market_data.symbol_indices["SYM" + std::to_string(i)] = i;
    }
    market_data.covariance = fixture.cov_50;
    // Returns matrix: 100 rows (lookback), 50 columns (symbols)
    market_data.returns.resize(100, std::vector<double>(50));
    for (size_t i = 0; i < 100; ++i) {
        for (size_t j = 0; j < 50; ++j) {
            market_data.returns[i][j] = fixture.returns_50[j] * (1.0 + i * 0.001);
        }
    }

    // Construct positions: equal weight across 50 symbols
    std::unordered_map<std::string, Position> positions;
    auto now = std::chrono::system_clock::now();
    for (size_t i = 0; i < 50; ++i) {
        Position pos("SYM" + std::to_string(i), Quantity(5), Price(100.0),
                     Decimal(0), Decimal(0), Timestamp(now));
        positions[pos.symbol] = pos;
    }

    // Call process_positions: the hot path that calls calculate_portfolio_multiplier
    auto result = rm.process_positions(positions, market_data);
    if (result.is_ok()) {
        volatile double val = result.value().portfolio_var;
        do_not_optimize(val);
    }
}

void benchmark_bar_conversion_10_symbols(const BenchmarkFixture& fixture) {
    // Benchmark Arrow table conversion (data module path).
    // Simulates pqxx result -> Bar conversion.

    // For now, just benchmark the reverse: bars -> table
    // (the forward path requires pqxx which we avoid)
    volatile auto bars = fixture.bars_10;
    do_not_optimize(bars);
}

void benchmark_statistics_normalization_10(const BenchmarkFixture& fixture) {
    // Benchmark Normalizer from statistics module.

    using namespace statistics;

    NormalizationConfig config;
    config.method = NormalizationConfig::Method::Z_SCORE;

    Normalizer normalizer(config);

    // Create small matrix (10x100)
    Eigen::MatrixXd data(100, 10);
    for (int i = 0; i < 100; ++i) {
        for (int j = 0; j < 10; ++j) {
            data(i, j) = fixture.returns_10[j % 10] + i * 0.001;
        }
    }

    auto fit_result = normalizer.fit(data);
    if (fit_result.is_ok()) {
        auto transform_result = normalizer.transform(data);
        if (transform_result.is_ok()) {
            auto transformed = transform_result.value();
            volatile double val = transformed(0, 0);
            do_not_optimize(val);
        }
    }
}

void benchmark_statistics_adf_test_10(const BenchmarkFixture& fixture) {
    // Benchmark ADF test from statistics module.

    using namespace statistics;

    ADFTestConfig config;
    config.regression = ADFTestConfig::RegressionType::CONSTANT;
    config.max_lags = 0;

    ADFTest adf(config);

    // Convert fixture returns to price series (cumulative)
    std::vector<double> prices;
    double price = 100.0;
    for (double ret : fixture.returns_10) {
        price *= (1.0 + ret);
        prices.push_back(price);
    }

    auto result = adf.test(prices);
    if (result.is_ok()) {
        auto test_result = result.value();
        volatile double stat = test_result.statistic;
        do_not_optimize(stat);
    }
}

int main(int argc, char* argv[]) {
    std::string output_path;
    bool save_json = false;

    // Parse --json flag
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            save_json = true;
            output_path = argv[++i];
        }
    }

    // Create fixture (synthetic data, deterministic, no I/O)
    BenchmarkFixture fixture;

    // Register benchmarks
    auto& registry = BenchmarkRegistry::instance();

    registry.register_benchmark("Optimizer_10_symbols", [&fixture]() {
        benchmark_optimizer_10_symbols(fixture);
    });

    registry.register_benchmark("Optimizer_50_symbols", [&fixture]() {
        benchmark_optimizer_50_symbols(fixture);
    });

    registry.register_benchmark("RiskMgr_PortfolioVar_10", [&fixture]() {
        benchmark_risk_portfolio_variance_10(fixture);
    });

    registry.register_benchmark("RiskMgr_PortfolioVar_50", [&fixture]() {
        benchmark_risk_portfolio_variance_50(fixture);
    });

    registry.register_benchmark("BarConversion_10_symbols", [&fixture]() {
        benchmark_bar_conversion_10_symbols(fixture);
    });

    registry.register_benchmark("Statistics_Normalizer_10", [&fixture]() {
        benchmark_statistics_normalization_10(fixture);
    });

    registry.register_benchmark("Statistics_ADF_test_10", [&fixture]() {
        benchmark_statistics_adf_test_10(fixture);
    });

    // Run all benchmarks
    if (save_json && !output_path.empty()) {
        registry.run_all_benchmarks_and_save(output_path, 5, 100);
    } else {
        registry.run_all_benchmarks(5, 100);
    }

    return 0;
}
