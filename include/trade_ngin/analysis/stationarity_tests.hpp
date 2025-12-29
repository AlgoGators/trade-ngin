#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include <vector>

namespace trade_ngin {
namespace analysis {

/**
 * @brief Type of regression for ADF test
 */
enum class ADFRegressionType {
    NO_CONSTANT,   // No constant term (no drift)
    CONSTANT,      // Constant term only (drift, no trend)
    CONSTANT_TREND // Constant and linear trend
};

/**
 * @brief Type of test for KPSS
 */
enum class KPSSType {
    LEVEL,  // Test for level stationarity
    TREND   // Test for trend stationarity
};

/**
 * @brief Result of Augmented Dickey-Fuller test
 */
struct ADFResult {
    double test_statistic;      // ADF test statistic
    double p_value;             // Approximate p-value
    int lags_used;              // Number of lags used in the test
    int n_obs;                  // Number of observations used
    double critical_value_1;    // Critical value at 1% significance
    double critical_value_5;    // Critical value at 5% significance
    double critical_value_10;   // Critical value at 10% significance
    bool is_stationary_1;       // Reject null at 1% level (stationary)
    bool is_stationary_5;       // Reject null at 5% level (stationary)
    bool is_stationary_10;      // Reject null at 10% level (stationary)
};

/**
 * @brief Result of KPSS test
 */
struct KPSSResult {
    double test_statistic;      // KPSS test statistic
    double p_value;             // Approximate p-value
    int lags_used;              // Number of lags used in the test
    int n_obs;                  // Number of observations used
    double critical_value_1;    // Critical value at 1% significance
    double critical_value_5;    // Critical value at 5% significance
    double critical_value_10;   // Critical value at 10% significance
    bool is_stationary_1;       // Fail to reject null at 1% level (stationary)
    bool is_stationary_5;       // Fail to reject null at 5% level (stationary)
    bool is_stationary_10;      // Fail to reject null at 10% level (stationary)
};

/**
 * @brief Augmented Dickey-Fuller test for stationarity
 *
 * Null hypothesis: Series has a unit root (non-stationary)
 * Alternative: Series is stationary
 *
 * Lower (more negative) test statistics provide stronger evidence against
 * the null hypothesis.
 *
 * @param data Time series data
 * @param max_lag Maximum number of lags to consider (0 = auto-select)
 * @param regression Type of regression to use
 * @return ADF test result
 */
Result<ADFResult> augmented_dickey_fuller_test(
    const std::vector<double>& data,
    int max_lag = 0,
    ADFRegressionType regression = ADFRegressionType::CONSTANT
);

/**
 * @brief ADF test on closing prices from Bar vector
 * @param bars Vector of OHLCV bars
 * @param max_lag Maximum number of lags (0 = auto-select)
 * @param regression Type of regression to use
 * @return ADF test result
 */
Result<ADFResult> augmented_dickey_fuller_test(
    const std::vector<Bar>& bars,
    int max_lag = 0,
    ADFRegressionType regression = ADFRegressionType::CONSTANT
);

/**
 * @brief KPSS test for stationarity
 *
 * Null hypothesis: Series is stationary
 * Alternative: Series has a unit root (non-stationary)
 *
 * Higher test statistics provide stronger evidence against the null hypothesis.
 * This test is complementary to ADF test (opposite null hypothesis).
 *
 * @param data Time series data
 * @param max_lag Maximum number of lags to consider (0 = auto-select)
 * @param test_type Type of stationarity to test
 * @return KPSS test result
 */
Result<KPSSResult> kpss_test(
    const std::vector<double>& data,
    int max_lag = 0,
    KPSSType test_type = KPSSType::LEVEL
);

/**
 * @brief KPSS test on closing prices from Bar vector
 * @param bars Vector of OHLCV bars
 * @param max_lag Maximum number of lags (0 = auto-select)
 * @param test_type Type of stationarity to test
 * @return KPSS test result
 */
Result<KPSSResult> kpss_test(
    const std::vector<Bar>& bars,
    int max_lag = 0,
    KPSSType test_type = KPSSType::LEVEL
);

/**
 * @brief Simple helper to check if series is stationary
 *
 * Uses both ADF and KPSS tests for robustness.
 * Series is considered stationary if ADF rejects null AND KPSS fails to reject null.
 *
 * @param data Time series data
 * @param significance Significance level (0.01, 0.05, or 0.10)
 * @return True if series appears stationary at the given significance level
 */
Result<bool> is_stationary(const std::vector<double>& data, double significance = 0.05);

} // namespace analysis
} // namespace trade_ngin
