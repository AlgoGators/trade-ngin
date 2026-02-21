#include "trade_ngin/statistics/tests/engle_granger_test.hpp"
#include "trade_ngin/statistics/statistics_utils.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {
namespace statistics {

EngleGrangerTest::EngleGrangerTest(EngleGrangerConfig config)
    : config_(config)
    , adf_test_(ADFTestConfig{}) {}

Result<TestResult> EngleGrangerTest::test(const std::vector<double>& y,
                                         const std::vector<double>& x) const {
    {
        auto valid_y = validation::validate_time_series(y, 20, "EngleGrangerTest");
        if (valid_y.is_error()) {
            return make_error<TestResult>(valid_y.error()->code(), valid_y.error()->what(), "EngleGrangerTest");
        }
        auto valid_x = validation::validate_time_series(x, 20, "EngleGrangerTest");
        if (valid_x.is_error()) {
            return make_error<TestResult>(valid_x.error()->code(), valid_x.error()->what(), "EngleGrangerTest");
        }
        if (y.size() != x.size()) {
            return make_error<TestResult>(
                ErrorCode::INVALID_ARGUMENT,
                "Series length mismatch: y has " + std::to_string(y.size()) +
                " observations, x has " + std::to_string(x.size()),
                "EngleGrangerTest"
            );
        }
    }

    DEBUG("[EngleGrangerTest::test] entry: n=" << y.size());

    // Step 1: OLS regression of y on x
    auto [beta, residuals] = ols_regression(y, x);

    // Step 2: ADF test on residuals
    auto adf_result = adf_test_.test(residuals);
    if (adf_result.is_error()) {
        return make_error<TestResult>(
            adf_result.error()->code(),
            adf_result.error()->what(),
            "EngleGrangerTest"
        );
    }

    TestResult result = adf_result.value();
    result.additional_stats["regression_coefficient"] = beta;

    // Modify interpretation for cointegration context
    if (result.reject_null) {
        result.interpretation = "Reject null hypothesis: Series appear to be cointegrated";
    } else {
        result.interpretation = "Cannot reject null hypothesis: No evidence of cointegration";
    }

    return Result<TestResult>(std::move(result));
}

std::pair<double, std::vector<double>> EngleGrangerTest::ols_regression(
    const std::vector<double>& y,
    const std::vector<double>& x) const {

    int n = y.size();

    // Calculate means
    double mean_x = utils::calculate_mean(x);
    double mean_y = utils::calculate_mean(y);

    // Calculate beta = cov(x,y) / var(x)
    double cov_xy = 0.0;
    double var_x = 0.0;
    for (int i = 0; i < n; ++i) {
        cov_xy += (x[i] - mean_x) * (y[i] - mean_y);
        var_x += (x[i] - mean_x) * (x[i] - mean_x);
    }
    double beta = cov_xy / var_x;
    double alpha = mean_y - beta * mean_x;

    // Calculate residuals
    std::vector<double> residuals(n);
    for (int i = 0; i < n; ++i) {
        residuals[i] = y[i] - (alpha + beta * x[i]);
    }

    return {beta, residuals};
}

} // namespace statistics
} // namespace trade_ngin
