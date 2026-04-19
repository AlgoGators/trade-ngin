#pragma once

#include "trade_ngin/statistics/statistics_common.hpp"

namespace trade_ngin {
namespace statistics {

/**
 * @brief Base class for all statistical tests
 */
class StatisticalTest {
public:
    virtual ~StatisticalTest() = default;

    /**
     * @brief Perform the statistical test
     * @param data Input time series data
     * @return Test result with statistics and interpretation
     */
    virtual Result<TestResult> test(const std::vector<double>& data) const = 0;

    /**
     * @brief Get test name
     */
    virtual std::string get_name() const = 0;
};

} // namespace statistics
} // namespace trade_ngin
