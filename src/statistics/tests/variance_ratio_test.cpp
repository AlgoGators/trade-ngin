#include "trade_ngin/statistics/tests/variance_ratio_test.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

namespace trade_ngin {
namespace statistics {

VarianceRatioTest::VarianceRatioTest(VarianceRatioConfig config)
    : config_(config) {}

double VarianceRatioTest::compute_vr(const std::vector<double>& data, int q) const {
    int T = static_cast<int>(data.size());

    // 1-period returns (first differences)
    double mean_r = (data.back() - data.front()) / (T - 1);

    // Variance of 1-period returns
    double var1 = 0.0;
    for (int t = 1; t < T; ++t) {
        double r = data[t] - data[t - 1] - mean_r;
        var1 += r * r;
    }
    var1 /= (T - 1);

    // Variance of q-period returns
    double varq = 0.0;
    int count = 0;
    for (int t = q; t < T; ++t) {
        double rq = data[t] - data[t - q] - q * mean_r;
        varq += rq * rq;
        count++;
    }
    varq /= count;

    return varq / (q * var1);
}

double VarianceRatioTest::compute_z_homo(const std::vector<double>& data, int q, double vr) const {
    int T = static_cast<int>(data.size()) - 1;
    // Asymptotic variance under homoskedasticity
    double avar = 2.0 * (2.0 * q - 1.0) * (q - 1.0) / (3.0 * q * T);
    return (vr - 1.0) / std::sqrt(avar);
}

double VarianceRatioTest::compute_z_hetero(const std::vector<double>& data, int q, double vr) const {
    int T = static_cast<int>(data.size());
    int nq = T - 1;

    double mean_r = (data.back() - data.front()) / nq;

    // Compute squared demeaned returns
    std::vector<double> r(nq);
    for (int t = 0; t < nq; ++t) {
        r[t] = data[t + 1] - data[t] - mean_r;
    }

    // Heteroskedasticity-robust asymptotic variance (Lo-MacKinlay 1988)
    double sum_r2_sq = 0.0;
    for (int t = 0; t < nq; ++t) {
        sum_r2_sq += r[t] * r[t];
    }

    double avar = 0.0;
    for (int j = 1; j < q; ++j) {
        double weight = 2.0 * (q - j) / q;
        weight *= weight;

        double delta_j = 0.0;
        double denom = sum_r2_sq * sum_r2_sq / (nq * nq);
        for (int t = j; t < nq; ++t) {
            delta_j += r[t] * r[t] * r[t - j] * r[t - j];
        }
        delta_j /= denom;

        avar += weight * delta_j / (nq * nq);
    }

    if (avar <= 0) return 0.0;
    return (vr - 1.0) / std::sqrt(avar);
}

Result<TestResult> VarianceRatioTest::test(const std::vector<double>& data) const {
    // Use first holding period for the single-test interface
    auto multi = test_multiple(data);
    if (multi.is_error()) {
        return make_error<TestResult>(multi.error()->code(), multi.error()->what(), "VarianceRatioTest");
    }

    const auto& res = multi.value();

    TestResult result;
    result.statistic = res.z_statistics.empty() ? 0.0 : res.z_statistics[0];
    result.p_value = res.p_values.empty() ? 1.0 : res.p_values[0];
    result.critical_value = 1.96; // Two-sided 5%
    result.reject_null = res.reject_random_walk;
    result.interpretation = res.interpretation;

    for (size_t i = 0; i < res.holding_periods.size(); ++i) {
        std::string prefix = "q" + std::to_string(res.holding_periods[i]) + "_";
        result.additional_stats[prefix + "vr"] = res.vr_statistics[i];
        result.additional_stats[prefix + "z"] = res.z_statistics[i];
        result.additional_stats[prefix + "p"] = res.p_values[i];
    }

    return Result<TestResult>(std::move(result));
}

Result<VarianceRatioResult> VarianceRatioTest::test_multiple(const std::vector<double>& data) const {
    {
        auto valid = validation::validate_time_series(data, 20, "VarianceRatioTest");
        if (valid.is_error()) {
            return make_error<VarianceRatioResult>(valid.error()->code(), valid.error()->what(), "VarianceRatioTest");
        }
    }

    DEBUG("[VarianceRatioTest::test_multiple] entry: n=" << data.size()
          << " periods=" << config_.holding_periods.size());

    VarianceRatioResult result;
    result.holding_periods = config_.holding_periods;
    result.reject_random_walk = false;

    for (int q : config_.holding_periods) {
        if (q < 2 || q >= static_cast<int>(data.size())) {
            result.vr_statistics.push_back(1.0);
            result.z_statistics.push_back(0.0);
            result.p_values.push_back(1.0);
            continue;
        }

        double vr = compute_vr(data, q);
        double z;
        if (config_.heteroskedasticity_robust) {
            z = compute_z_hetero(data, q, vr);
        } else {
            z = compute_z_homo(data, q, vr);
        }

        // Two-sided p-value via erfc
        double p = std::erfc(std::abs(z) / std::sqrt(2.0));

        result.vr_statistics.push_back(vr);
        result.z_statistics.push_back(z);
        result.p_values.push_back(p);

        if (p < config_.significance_level) {
            result.reject_random_walk = true;
        }
    }

    if (result.reject_random_walk) {
        // Determine if mean-reverting or momentum
        bool all_below = true, all_above = true;
        for (double vr : result.vr_statistics) {
            if (vr >= 1.0) all_below = false;
            if (vr <= 1.0) all_above = false;
        }
        if (all_below) {
            result.interpretation = "Reject random walk: Evidence of mean reversion (VR < 1)";
        } else if (all_above) {
            result.interpretation = "Reject random walk: Evidence of momentum/trending (VR > 1)";
        } else {
            result.interpretation = "Reject random walk: Mixed variance ratio pattern";
        }
    } else {
        result.interpretation = "Cannot reject random walk hypothesis";
    }

    DEBUG("[VarianceRatioTest::test_multiple] exit: reject=" << result.reject_random_walk);

    return Result<VarianceRatioResult>(std::move(result));
}

} // namespace statistics
} // namespace trade_ngin
