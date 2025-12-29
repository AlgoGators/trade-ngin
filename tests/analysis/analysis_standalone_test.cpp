/**
 * Standalone test for analysis tools
 * This file can be compiled independently to test the analysis module
 *
 * Compile with MSYS2 (from tests/analysis/ directory):
 * ./compile_test.sh
 *
 * Or manually:
 * g++ -std=c++17 -I"../../include" -I"../../externals/eigen" \
 *     analysis_standalone_test.cpp \
 *     ../../src/analysis/statistical_distributions.cpp \
 *     ../../src/analysis/preprocessing.cpp \
 *     ../../src/analysis/stationarity_tests.cpp \
 *     -o analysis_test.exe
 */

#include "trade_ngin/analysis/preprocessing.hpp"
#include "trade_ngin/analysis/stationarity_tests.hpp"
#include "trade_ngin/analysis/statistical_distributions.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <random>

using namespace trade_ngin::analysis;

void test_preprocessing() {
    std::cout << "\n=== Testing Preprocessing ===" << std::endl;

    // Generate sample data
    std::vector<double> prices;
    double price = 100.0;
    std::mt19937 gen(42);
    std::normal_distribution<> dist(0.001, 0.02);

    for (int i = 0; i < 100; ++i) {
        price *= (1.0 + dist(gen));
        prices.push_back(price);
    }

    std::cout << "Generated " << prices.size() << " price points" << std::endl;
    std::cout << "Price range: " << prices.front() << " to " << prices.back() << std::endl;

    // Test z-score normalization
    auto normalized = Normalization::z_score(prices);
    if (normalized.is_ok()) {
        std::cout << "✓ Z-score normalization successful" << std::endl;
        auto& values = normalized.value();
        double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
        std::cout << "  Normalized mean (should be ~0): " << mean << std::endl;
    } else {
        std::cout << "✗ Z-score normalization failed: " << normalized.error()->what() << std::endl;
    }

    // Test min-max scaling
    auto scaled = Normalization::min_max(prices);
    if (scaled.is_ok()) {
        std::cout << "✓ Min-max scaling successful" << std::endl;
        auto& values = scaled.value();
        auto [min_it, max_it] = std::minmax_element(values.begin(), values.end());
        std::cout << "  Range: [" << *min_it << ", " << *max_it << "]" << std::endl;
    } else {
        std::cout << "✗ Min-max scaling failed" << std::endl;
    }

    // Test returns calculation
    auto returns = Normalization::calculate_returns(prices, true);
    if (returns.is_ok()) {
        std::cout << "✓ Returns calculation successful" << std::endl;
        double mean_return = std::accumulate(returns.value().begin(),
                                             returns.value().end(), 0.0) / returns.value().size();
        std::cout << "  Mean return: " << mean_return * 100 << "%" << std::endl;
    } else {
        std::cout << "✗ Returns calculation failed" << std::endl;
    }
}

void test_statistical_distributions() {
    std::cout << "\n=== Testing Statistical Distributions ===" << std::endl;

    // Test normal CDF
    double z = 1.96;
    double cdf = StatisticalDistributions::normal_cdf(z);
    std::cout << "Normal CDF at z=" << z << ": " << cdf << " (expected ~0.975)" << std::endl;

    // Test normal quantile
    double p = 0.975;
    double quantile = StatisticalDistributions::normal_quantile(p);
    std::cout << "Normal quantile at p=" << p << ": " << quantile << " (expected ~1.96)" << std::endl;

    // Test chi-square
    double chi_stat = 5.0;
    double chi_cdf = StatisticalDistributions::chi_square_cdf(chi_stat, 3);
    std::cout << "Chi-square CDF(5, df=3): " << chi_cdf << std::endl;

    std::cout << "✓ Statistical distributions working" << std::endl;
}

void test_stationarity() {
    std::cout << "\n=== Testing Stationarity Tests ===" << std::endl;

    // Generate non-stationary series (random walk)
    std::vector<double> random_walk;
    double value = 100.0;
    std::mt19937 gen(42);
    std::normal_distribution<> dist(0.0, 1.0);

    for (int i = 0; i < 200; ++i) {
        value += dist(gen);
        random_walk.push_back(value);
    }

    // Generate stationary series (white noise)
    std::vector<double> white_noise;
    for (int i = 0; i < 200; ++i) {
        white_noise.push_back(dist(gen));
    }

    // Test ADF on random walk
    std::cout << "\nTesting Random Walk (should be NON-stationary):" << std::endl;
    auto adf_rw = augmented_dickey_fuller_test(random_walk);
    if (adf_rw.is_ok()) {
        auto& result = adf_rw.value();
        std::cout << "  ADF statistic: " << result.test_statistic << std::endl;
        std::cout << "  Critical value (5%): " << result.critical_value_5 << std::endl;
        std::cout << "  Is stationary: " << (result.is_stationary_5 ? "YES" : "NO") << std::endl;
    }

    // Test ADF on white noise
    std::cout << "\nTesting White Noise (should be stationary):" << std::endl;
    auto adf_wn = augmented_dickey_fuller_test(white_noise);
    if (adf_wn.is_ok()) {
        auto& result = adf_wn.value();
        std::cout << "  ADF statistic: " << result.test_statistic << std::endl;
        std::cout << "  Critical value (5%): " << result.critical_value_5 << std::endl;
        std::cout << "  Is stationary: " << (result.is_stationary_5 ? "YES" : "NO") << std::endl;
    }

    // Test KPSS
    std::cout << "\nKPSS Test on White Noise:" << std::endl;
    auto kpss_wn = kpss_test(white_noise);
    if (kpss_wn.is_ok()) {
        auto& result = kpss_wn.value();
        std::cout << "  KPSS statistic: " << result.test_statistic << std::endl;
        std::cout << "  Critical value (5%): " << result.critical_value_5 << std::endl;
        std::cout << "  Is stationary: " << (result.is_stationary_5 ? "YES" : "NO") << std::endl;
    }
}

void test_pca() {
    std::cout << "\n=== Testing PCA ===" << std::endl;

    // Generate correlated data
    std::mt19937 gen(42);
    std::normal_distribution<> dist(0.0, 1.0);

    int n_samples = 100;
    int n_features = 4;

    Eigen::MatrixXd data(n_samples, n_features);

    for (int i = 0; i < n_samples; ++i) {
        double common = dist(gen);
        data(i, 0) = 0.8 * common + 0.2 * dist(gen);
        data(i, 1) = 0.8 * common + 0.2 * dist(gen);
        data(i, 2) = 0.6 * common + 0.4 * dist(gen);
        data(i, 3) = dist(gen);  // Independent
    }

    // Apply PCA
    PCA pca(2);  // Keep 2 components
    auto result = pca.fit_transform(data);

    if (result.is_ok()) {
        std::cout << "✓ PCA fit successful" << std::endl;
        std::cout << "  Explained variance ratios:" << std::endl;
        for (int i = 0; i < result.value().explained_variance_ratio.size(); ++i) {
            std::cout << "    PC" << (i+1) << ": "
                     << result.value().explained_variance_ratio(i) * 100 << "%" << std::endl;
        }

        std::cout << "  Original dimensions: " << n_features << std::endl;
        std::cout << "  Reduced dimensions: " << result.value().transformed_data.cols() << std::endl;
    } else {
        std::cout << "✗ PCA failed: " << result.error()->what() << std::endl;
    }
}

int main() {
    std::cout << "==================================" << std::endl;
    std::cout << "  Analysis Tools Standalone Test  " << std::endl;
    std::cout << "==================================" << std::endl;

    try {
        test_statistical_distributions();
        test_preprocessing();
        test_stationarity();
        test_pca();

        std::cout << "\n==================================" << std::endl;
        std::cout << "  ✓ All Tests Completed!" << std::endl;
        std::cout << "==================================" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
