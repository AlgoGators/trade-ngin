#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include "trade_ngin/analysis/stationarity_tests.hpp"
#include <vector>
#include <Eigen/Dense>

namespace trade_ngin {
namespace analysis {

/**
 * @brief Result of Engle-Granger two-step cointegration test
 */
struct EngleGrangerResult {
    double cointegration_coefficient;  // Beta in y = alpha + beta*x + residual
    double intercept;                  // Alpha in y = alpha + beta*x + residual
    ADFResult adf_result;              // ADF test on residuals
    bool is_cointegrated_1;            // Cointegrated at 1% level
    bool is_cointegrated_5;            // Cointegrated at 5% level
    bool is_cointegrated_10;           // Cointegrated at 10% level
    std::vector<double> residuals;     // Regression residuals
};

/**
 * @brief Result of Johansen cointegration test
 */
struct JohansenResult {
    Eigen::VectorXd eigenvalues;            // Eigenvalues from canonical correlation
    Eigen::MatrixXd eigenvectors;           // Cointegrating vectors (each column is a vector)
    std::vector<double> trace_statistics;   // Trace test statistics
    std::vector<double> max_eigen_statistics; // Maximum eigenvalue test statistics
    std::vector<double> trace_critical_values_5;  // Critical values for trace test at 5%
    std::vector<double> max_eigen_critical_values_5; // Critical values for max eigenvalue test at 5%
    int rank_trace;                         // Cointegration rank from trace test (5% level)
    int rank_max_eigen;                     // Cointegration rank from max eigenvalue test (5% level)
};

/**
 * @brief Engle-Granger two-step cointegration test
 *
 * Tests for cointegration between two time series using the Engle-Granger method:
 * 1. Run OLS regression: y = alpha + beta*x + residual
 * 2. Test residuals for stationarity using ADF test
 *
 * If residuals are stationary, the series are cointegrated.
 *
 * @param y Dependent variable time series
 * @param x Independent variable time series
 * @param max_lag Maximum lag for ADF test (0 = auto-select)
 * @return Engle-Granger test result
 */
Result<EngleGrangerResult> engle_granger_test(
    const std::vector<double>& y,
    const std::vector<double>& x,
    int max_lag = 0
);

/**
 * @brief Engle-Granger test using closing prices from Bar vectors
 * @param y_bars Dependent variable bars
 * @param x_bars Independent variable bars
 * @param max_lag Maximum lag for ADF test (0 = auto-select)
 * @return Engle-Granger test result
 */
Result<EngleGrangerResult> engle_granger_test(
    const std::vector<Bar>& y_bars,
    const std::vector<Bar>& x_bars,
    int max_lag = 0
);

/**
 * @brief Johansen cointegration test
 *
 * Tests for cointegration among multiple time series using the Johansen method.
 * This test can identify multiple cointegrating relationships.
 *
 * The test provides two test statistics:
 * - Trace test: Tests H0: rank <= r vs H1: rank > r
 * - Maximum eigenvalue test: Tests H0: rank = r vs H1: rank = r+1
 *
 * @param data Matrix where each column is a time series (n_obs x n_series)
 * @param max_lag Maximum lag for VAR model (0 = auto-select)
 * @param deterministic_trend Type of deterministic trend (0=none, 1=constant, 2=trend)
 * @return Johansen test result
 */
Result<JohansenResult> johansen_test(
    const Eigen::MatrixXd& data,
    int max_lag = 0,
    int deterministic_trend = 1
);

/**
 * @brief Johansen test using vector of price series
 * @param series Vector of time series (each vector is one series)
 * @param max_lag Maximum lag for VAR model (0 = auto-select)
 * @param deterministic_trend Type of deterministic trend (0=none, 1=constant, 2=trend)
 * @return Johansen test result
 */
Result<JohansenResult> johansen_test(
    const std::vector<std::vector<double>>& series,
    int max_lag = 0,
    int deterministic_trend = 1
);

/**
 * @brief Johansen test using vector of Bar vectors
 * @param bar_series Vector of Bar vectors (closing prices used)
 * @param max_lag Maximum lag for VAR model (0 = auto-select)
 * @param deterministic_trend Type of deterministic trend (0=none, 1=constant, 2=trend)
 * @return Johansen test result
 */
Result<JohansenResult> johansen_test(
    const std::vector<std::vector<Bar>>& bar_series,
    int max_lag = 0,
    int deterministic_trend = 1
);

/**
 * @brief Simple helper to check if two series are cointegrated
 * @param y First time series
 * @param x Second time series
 * @param significance Significance level (0.01, 0.05, or 0.10)
 * @return True if series are cointegrated at the given significance level
 */
Result<bool> is_cointegrated(
    const std::vector<double>& y,
    const std::vector<double>& x,
    double significance = 0.05
);

} // namespace analysis
} // namespace trade_ngin
