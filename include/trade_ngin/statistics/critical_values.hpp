#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace trade_ngin {
namespace statistics {
namespace critical_values {

// ============================================================================
// Log-Sum-Exp Utilities
// ============================================================================

/**
 * @brief Numerically stable log(exp(a) + exp(b))
 */
inline double log_sum_exp(double a, double b) {
    if (a == -std::numeric_limits<double>::infinity()) return b;
    if (b == -std::numeric_limits<double>::infinity()) return a;
    double mx = std::max(a, b);
    return mx + std::log1p(std::exp(-std::abs(a - b)));
}

/**
 * @brief Numerically stable log(sum(exp(values[0..n-1])))
 */
inline double log_sum_exp(const double* values, int n) {
    if (n == 0) return -std::numeric_limits<double>::infinity();
    double mx = values[0];
    for (int i = 1; i < n; ++i) {
        if (values[i] > mx) mx = values[i];
    }
    if (mx == -std::numeric_limits<double>::infinity()) return mx;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        sum += std::exp(values[i] - mx);
    }
    return mx + std::log(sum);
}

// ============================================================================
// ADF Critical Values — MacKinnon (1996)
// ============================================================================
// Tables indexed by [regression_type][sample_size_idx][significance_idx]
// Regression types: 0=no_constant, 1=constant, 2=constant_trend
// Sample size indices: 0=25, 1=50, 2=100, 3=250, 4=500, 5=inf
// Significance indices: 0=1%, 1=5%, 2=10%

constexpr int ADF_N_REG_TYPES = 3;
constexpr int ADF_N_SAMPLE_SIZES = 6;
constexpr int ADF_N_SIG_LEVELS = 3;

constexpr int adf_sample_sizes[ADF_N_SAMPLE_SIZES] = {25, 50, 100, 250, 500, 100000};

// No constant (regression type 0)
constexpr double adf_cv_no_constant[ADF_N_SAMPLE_SIZES][ADF_N_SIG_LEVELS] = {
    // 1%,     5%,     10%
    {-2.66, -1.95, -1.60},  // n=25
    {-2.62, -1.95, -1.61},  // n=50
    {-2.60, -1.95, -1.61},  // n=100
    {-2.58, -1.95, -1.62},  // n=250
    {-2.58, -1.95, -1.62},  // n=500
    {-2.58, -1.95, -1.62},  // n=inf
};

// Constant (regression type 1)
constexpr double adf_cv_constant[ADF_N_SAMPLE_SIZES][ADF_N_SIG_LEVELS] = {
    // 1%,     5%,     10%
    {-3.75, -3.00, -2.63},  // n=25
    {-3.58, -2.93, -2.60},  // n=50
    {-3.51, -2.89, -2.58},  // n=100
    {-3.46, -2.87, -2.57},  // n=250
    {-3.44, -2.87, -2.57},  // n=500
    {-3.43, -2.86, -2.57},  // n=inf
};

// Constant + trend (regression type 2)
constexpr double adf_cv_constant_trend[ADF_N_SAMPLE_SIZES][ADF_N_SIG_LEVELS] = {
    // 1%,     5%,     10%
    {-4.38, -3.60, -3.24},  // n=25
    {-4.15, -3.50, -3.18},  // n=50
    {-4.04, -3.45, -3.15},  // n=100
    {-3.99, -3.43, -3.13},  // n=250
    {-3.98, -3.42, -3.13},  // n=500
    {-3.96, -3.41, -3.12},  // n=inf
};

/**
 * @brief Interpolate ADF critical value by sample size
 * @param n_obs Number of observations
 * @param regression_type 0=no_constant, 1=constant, 2=constant_trend
 * @param significance 0.01, 0.05, or 0.10
 * @return Interpolated critical value
 */
inline double interpolate_adf_cv(int n_obs, int regression_type, double significance) {
    // Select significance index
    int sig_idx;
    if (significance <= 0.01) sig_idx = 0;
    else if (significance <= 0.05) sig_idx = 1;
    else sig_idx = 2;

    // Select table
    const double (*table)[ADF_N_SIG_LEVELS];
    switch (regression_type) {
        case 0: table = adf_cv_no_constant; break;
        case 2: table = adf_cv_constant_trend; break;
        default: table = adf_cv_constant; break;
    }

    // Find bracketing sample sizes
    if (n_obs <= adf_sample_sizes[0]) {
        return table[0][sig_idx];
    }
    if (n_obs >= adf_sample_sizes[ADF_N_SAMPLE_SIZES - 1]) {
        return table[ADF_N_SAMPLE_SIZES - 1][sig_idx];
    }

    for (int i = 0; i < ADF_N_SAMPLE_SIZES - 1; ++i) {
        if (n_obs >= adf_sample_sizes[i] && n_obs <= adf_sample_sizes[i + 1]) {
            double t = static_cast<double>(n_obs - adf_sample_sizes[i]) /
                       static_cast<double>(adf_sample_sizes[i + 1] - adf_sample_sizes[i]);
            return table[i][sig_idx] * (1.0 - t) + table[i + 1][sig_idx] * t;
        }
    }

    // Fallback
    return table[ADF_N_SAMPLE_SIZES - 1][sig_idx];
}

// ============================================================================
// KPSS Critical Values — Kwiatkowski et al. (1992)
// ============================================================================

/**
 * @brief KPSS critical values
 * @param significance 0.01, 0.05, or 0.10
 * @param has_trend Whether regression includes a trend
 * @return Critical value
 */
inline double kpss_critical_value(double significance, bool has_trend) {
    if (!has_trend) {
        // Level stationarity
        if (significance <= 0.01) return 0.739;
        if (significance <= 0.05) return 0.463;
        if (significance <= 0.10) return 0.347;
        return 0.347;  // default to 10%
    } else {
        // Trend stationarity
        if (significance <= 0.01) return 0.216;
        if (significance <= 0.05) return 0.146;
        if (significance <= 0.10) return 0.119;
        return 0.119;  // default to 10%
    }
}

// ============================================================================
// Johansen Trace Test Critical Values — Osterwald-Lenum (1992)
// ============================================================================
// Tables for trace statistic with intercept (no trend) in VAR
// Indexed by [n_series - 2][rank] for 5% and 1%

// 5% critical values for trace test
// Row index = n_series - 2 (so row 0 = 2 series, row 1 = 3 series, etc.)
// Column index = rank being tested (r=0, r=1, ...)
constexpr double johansen_trace_5pct[][5] = {
    // n=2: r=0, r=1
    {15.41, 3.76, 0.0, 0.0, 0.0},
    // n=3: r=0, r=1, r=2
    {29.68, 15.41, 3.76, 0.0, 0.0},
    // n=4: r=0, r=1, r=2, r=3
    {47.21, 29.68, 15.41, 3.76, 0.0},
    // n=5: r=0, r=1, r=2, r=3, r=4
    {68.52, 47.21, 29.68, 15.41, 3.76},
};

// 1% critical values for trace test
constexpr double johansen_trace_1pct[][5] = {
    // n=2: r=0, r=1
    {20.04, 6.65, 0.0, 0.0, 0.0},
    // n=3: r=0, r=1, r=2
    {35.65, 20.04, 6.65, 0.0, 0.0},
    // n=4: r=0, r=1, r=2, r=3
    {54.46, 35.65, 20.04, 6.65, 0.0},
    // n=5: r=0, r=1, r=2, r=3, r=4
    {76.07, 54.46, 35.65, 20.04, 6.65},
};

/**
 * @brief Get Johansen trace test critical values
 * @param n_series Number of series (2 to 5 supported directly)
 * @param significance 0.01 or 0.05
 * @return Vector of critical values for each rank test
 */
inline std::vector<double> johansen_trace_critical_values(int n_series, double significance) {
    std::vector<double> cv(n_series);

    if (n_series >= 2 && n_series <= 5) {
        int row = n_series - 2;
        const auto& table = (significance <= 0.01) ? johansen_trace_1pct : johansen_trace_5pct;
        for (int i = 0; i < n_series; ++i) {
            cv[i] = table[row][i];
        }
    } else {
        // Fallback approximation for n_series > 5
        for (int i = 0; i < n_series; ++i) {
            int remaining = n_series - i;
            double base = (significance <= 0.01) ? 6.65 : 3.76;
            cv[i] = base + (remaining - 1) * ((significance <= 0.01) ? 17.0 : 14.0);
        }
    }

    return cv;
}

} // namespace critical_values
} // namespace statistics
} // namespace trade_ngin
