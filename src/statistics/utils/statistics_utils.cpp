#include "trade_ngin/statistics/statistics_utils.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace trade_ngin {
namespace statistics {
namespace utils {

// Calculate mean of a vector
double calculate_mean(const std::vector<double>& data) {
    if (data.empty()) return 0.0;
    return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
}

// Calculate variance
double calculate_variance(const std::vector<double>& data, double mean) {
    if (data.size() <= 1) return 0.0;
    double sum_sq = 0.0;
    for (double val : data) {
        double diff = val - mean;
        sum_sq += diff * diff;
    }
    return sum_sq / (data.size() - 1);
}

// Calculate standard deviation
double calculate_std(const std::vector<double>& data, double mean) {
    return std::sqrt(calculate_variance(data, mean));
}

// Calculate median
double calculate_median(std::vector<double> data) {
    if (data.empty()) return 0.0;
    size_t n = data.size();
    std::nth_element(data.begin(), data.begin() + n / 2, data.end());
    if (n % 2 == 0) {
        double median1 = data[n / 2];
        std::nth_element(data.begin(), data.begin() + n / 2 - 1, data.end());
        return (median1 + data[n / 2 - 1]) / 2.0;
    }
    return data[n / 2];
}

// Calculate IQR (interquartile range)
double calculate_iqr(std::vector<double> data) {
    if (data.size() < 4) return 0.0;
    std::sort(data.begin(), data.end());
    size_t n = data.size();
    size_t q1_idx = n / 4;
    size_t q3_idx = 3 * n / 4;
    return data[q3_idx] - data[q1_idx];
}

// Autocorrelation function
std::vector<double> autocorrelation(const std::vector<double>& data, int max_lag) {
    size_t n = data.size();
    double mean = calculate_mean(data);
    double variance = calculate_variance(data, mean);

    std::vector<double> acf(max_lag + 1);
    for (int lag = 0; lag <= max_lag; ++lag) {
        double sum = 0.0;
        for (size_t t = lag; t < n; ++t) {
            sum += (data[t] - mean) * (data[t - lag] - mean);
        }
        acf[lag] = sum / (n * variance);
    }
    return acf;
}

// Difference a time series
std::vector<double> difference(const std::vector<double>& data, int order) {
    std::vector<double> result = data;
    for (int i = 0; i < order; ++i) {
        std::vector<double> diff;
        for (size_t j = 1; j < result.size(); ++j) {
            diff.push_back(result[j] - result[j - 1]);
        }
        result = diff;
    }
    return result;
}

} // namespace utils
} // namespace statistics
} // namespace trade_ngin
