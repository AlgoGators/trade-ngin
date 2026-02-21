#pragma once

#include "trade_ngin/statistics/base/statistical_test.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"

namespace trade_ngin {
namespace statistics {

/**
 * @brief Augmented Dickey-Fuller test for stationarity
 */
class ADFTest : public StatisticalTest {
public:
    explicit ADFTest(ADFTestConfig config);

    Result<TestResult> test(const std::vector<double>& data) const override;
    std::string get_name() const override { return "Augmented Dickey-Fuller Test"; }

private:
    ADFTestConfig config_;

    // Helper methods
    int select_lag_order(const std::vector<double>& data) const;
    double calculate_critical_value(int n_obs, double significance) const;
};

} // namespace statistics
} // namespace trade_ngin
