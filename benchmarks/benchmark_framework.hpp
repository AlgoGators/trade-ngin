// benchmarks/benchmark_framework.hpp
//
// A self-contained, minimal benchmark harness for trade-ngin.
//
// Purpose: Measure performance of refactored code with reproducible results.
// Why median/p95, not mean: Scheduler noise creates outliers that drag mean up,
// making unchanged code look slower. Median is stable; p95 catches tail regressions.
//
// Why do_not_optimize: Compiler optimizations can eliminate workloads entirely.
// An asm barrier forces the compiler to measure real code, not constant-folding.

#pragma once

#include <chrono>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace trade_ngin::bench {

// Prevent compiler from optimizing away the benchmark workload.
// Uses inline asm barrier that tells compiler:
// "this value might have side effects, do not optimize it away".
template <typename T>
inline void do_not_optimize(T& value) {
    asm volatile("" : "+r,m"(value) : : "memory");
}

struct BenchmarkResult {
    std::string name;
    int64_t iterations;
    int64_t median_ns;
    int64_t p95_ns;

    nlohmann::json to_json() const {
        return nlohmann::json{
            {"name", name},
            {"iterations", iterations},
            {"median_ns", median_ns},
            {"p95_ns", p95_ns}
        };
    }
};

class Benchmark {
public:
    // Register a benchmark by name with a callable workload.
    // The callable is executed 'iterations' times, timed.
    explicit Benchmark(const std::string& name, std::function<void()> fn)
        : name_(name), fn_(fn) {}

    // Run the benchmark: warmup phase + N timed iterations
    BenchmarkResult run(int warmup_iterations = 5, int timed_iterations = 100) {
        // Warmup: let caches warm, branch predictors stabilize
        for (int i = 0; i < warmup_iterations; ++i) {
            fn_();
        }

        // Timed iterations: record each one
        std::vector<int64_t> times;
        times.reserve(timed_iterations);

        for (int i = 0; i < timed_iterations; ++i) {
            auto start = std::chrono::steady_clock::now();
            fn_();
            auto end = std::chrono::steady_clock::now();

            int64_t elapsed_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            times.push_back(elapsed_ns);
        }

        // Compute median and p95 from sorted times
        std::sort(times.begin(), times.end());

        int64_t median = times[times.size() / 2];
        int64_t p95 = times[static_cast<int>(times.size() * 0.95)];

        return BenchmarkResult{name_, static_cast<int64_t>(timed_iterations), median, p95};
    }

private:
    std::string name_;
    std::function<void()> fn_;
};

class BenchmarkRegistry {
public:
    static BenchmarkRegistry& instance() {
        static BenchmarkRegistry registry;
        return registry;
    }

    void register_benchmark(const std::string& name, std::function<void()> fn) {
        benchmarks_[name] = fn;
    }

    void run_all_benchmarks(int warmup = 5, int timed = 100) {
        std::vector<BenchmarkResult> results;

        std::cout << "\n"
                  << std::string(70, '=') << "\n";
        std::cout << "Running " << benchmarks_.size() << " benchmarks...\n";
        std::cout << std::string(70, '=') << "\n\n";

        for (const auto& [name, fn] : benchmarks_) {
            Benchmark bench(name, fn);
            auto result = bench.run(warmup, timed);
            results.push_back(result);

            // Print human-readable line
            std::cout << std::left << std::setw(40) << name << " | "
                      << std::right << std::setw(12) << result.median_ns << " ns (median) | "
                      << std::setw(12) << result.p95_ns << " ns (p95)\n";
        }

        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "Benchmark Summary\n";
        std::cout << std::string(70, '=') << "\n\n";

        print_json_results(results);
    }

    void run_all_benchmarks_and_save(const std::string& output_path, int warmup = 5,
                                     int timed = 100) {
        std::vector<BenchmarkResult> results;

        std::cout << "\n"
                  << std::string(70, '=') << "\n";
        std::cout << "Running " << benchmarks_.size() << " benchmarks...\n";
        std::cout << std::string(70, '=') << "\n\n";

        for (const auto& [name, fn] : benchmarks_) {
            Benchmark bench(name, fn);
            auto result = bench.run(warmup, timed);
            results.push_back(result);

            std::cout << std::left << std::setw(40) << name << " | "
                      << std::right << std::setw(12) << result.median_ns << " ns (median) | "
                      << std::setw(12) << result.p95_ns << " ns (p95)\n";
        }

        std::cout << "\n" << std::string(70, '=') << "\n";
        std::cout << "Saving results to: " << output_path << "\n";
        std::cout << std::string(70, '=') << "\n\n";

        // Write JSON to file
        nlohmann::json json_results = nlohmann::json::array();
        for (const auto& result : results) {
            json_results.push_back(result.to_json());
        }

        std::ofstream out(output_path);
        if (out.is_open()) {
            out << json_results.dump(2) << "\n";
            out.close();
            std::cout << "Results saved successfully.\n";
        } else {
            std::cerr << "Failed to open output file: " << output_path << "\n";
        }
    }

private:
    std::map<std::string, std::function<void()>> benchmarks_;

    void print_json_results(const std::vector<BenchmarkResult>& results) {
        nlohmann::json json_results = nlohmann::json::array();
        for (const auto& result : results) {
            json_results.push_back(result.to_json());
        }

        std::cout << json_results.dump(2) << "\n";
    }
};

}  // namespace trade_ngin::bench
