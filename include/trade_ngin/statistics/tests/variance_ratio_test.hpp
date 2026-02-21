#pragma once

#include "trade_ngin/statistics/base/statistical_test.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"

namespace trade_ngin {
namespace statistics {

class VarianceRatioTest : public StatisticalTest {
public:
    explicit VarianceRatioTest(VarianceRatioConfig config);

    Result<TestResult> test(const std::vector<double>& data) const override;
    std::string get_name() const override { return "Variance Ratio Test"; }

    Result<VarianceRatioResult> test_multiple(const std::vector<double>& data) const;

private:
    VarianceRatioConfig config_;

    double compute_vr(const std::vector<double>& data, int q) const;
    double compute_z_homo(const std::vector<double>& data, int q, double vr) const;
    double compute_z_hetero(const std::vector<double>& data, int q, double vr) const;
};

} // namespace statistics
} // namespace trade_ngin
