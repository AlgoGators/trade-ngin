#pragma once

#include "trade_ngin/statistics/base/statistical_test.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"

namespace trade_ngin {
namespace statistics {

class PhillipsPerronTest : public StatisticalTest {
public:
    explicit PhillipsPerronTest(PhillipsPerronConfig config);

    Result<TestResult> test(const std::vector<double>& data) const override;
    std::string get_name() const override { return "Phillips-Perron Test"; }

private:
    PhillipsPerronConfig config_;

    int select_bandwidth(int n_obs) const;
    double kernel_weight(int lag, int bandwidth) const;
};

} // namespace statistics
} // namespace trade_ngin
