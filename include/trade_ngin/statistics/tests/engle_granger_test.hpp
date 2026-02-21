#pragma once

#include "trade_ngin/statistics/tests/adf_test.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"

namespace trade_ngin {
namespace statistics {

/**
 * @brief Engle-Granger two-step cointegration test
 */
class EngleGrangerTest {
public:
    explicit EngleGrangerTest(EngleGrangerConfig config);

    /**
     * @brief Test for pairwise cointegration
     * @param y Dependent variable time series
     * @param x Independent variable time series
     * @return Test result
     */
    Result<TestResult> test(const std::vector<double>& y,
                           const std::vector<double>& x) const;

private:
    EngleGrangerConfig config_;
    ADFTest adf_test_;

    // Helper: perform OLS regression
    std::pair<double, std::vector<double>> ols_regression(
        const std::vector<double>& y,
        const std::vector<double>& x) const;
};

} // namespace statistics
} // namespace trade_ngin
