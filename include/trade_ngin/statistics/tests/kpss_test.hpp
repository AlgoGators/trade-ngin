#pragma once

#include "trade_ngin/statistics/base/statistical_test.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"

namespace trade_ngin {
namespace statistics {

/**
 * @brief KPSS test for stationarity
 */
class KPSSTest : public StatisticalTest {
public:
    explicit KPSSTest(KPSSTestConfig config);

    Result<TestResult> test(const std::vector<double>& data) const override;
    std::string get_name() const override { return "KPSS Test"; }

private:
    KPSSTestConfig config_;

    int select_lag_order(int n_obs) const;
    double calculate_critical_value(double significance, bool has_trend) const;
};

} // namespace statistics
} // namespace trade_ngin
