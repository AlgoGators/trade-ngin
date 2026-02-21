#pragma once

#include <vector>

namespace trade_ngin {
namespace statistics {
namespace utils {

// Calculate mean of a vector
double calculate_mean(const std::vector<double>& data);

// Calculate variance
double calculate_variance(const std::vector<double>& data, double mean);

// Calculate standard deviation
double calculate_std(const std::vector<double>& data, double mean);

// Calculate median
double calculate_median(std::vector<double> data);

// Calculate IQR (interquartile range)
double calculate_iqr(std::vector<double> data);

// Autocorrelation function
std::vector<double> autocorrelation(const std::vector<double>& data, int max_lag);

// Difference a time series
std::vector<double> difference(const std::vector<double>& data, int order = 1);

} // namespace utils
} // namespace statistics
} // namespace trade_ngin
