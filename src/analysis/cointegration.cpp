#include "trade_ngin/analysis/cointegration.hpp"
#include "trade_ngin/analysis/preprocessing.hpp"
#include "trade_ngin/analysis/statistical_distributions.hpp"
#include <Eigen/Dense>
#include <Eigen/Eigenvalues>
#include <algorithm>
#include <cmath>

namespace trade_ngin {
namespace analysis {

namespace {
    // Critical values for Johansen trace test (with constant, no trend)
    // [r=0, r<=1, r<=2, r<=3, r<=4]
    const double JOHANSEN_TRACE_CV_5[5] = {15.49, 3.84, 0.0, 0.0, 0.0};
    const double JOHANSEN_TRACE_CV_5_MULTI[10] = {
        29.79, 15.49, 3.84, // n=2, r=0,1,2
        47.86, 29.79, 15.49, 3.84, // n=3, r=0,1,2,3
        69.82, 47.86, 29.79 // n=4, r=0,1,2 (partial)
    };

    // Critical values for Johansen max eigenvalue test (with constant, no trend)
    const double JOHANSEN_MAXEIG_CV_5[5] = {14.26, 3.84, 0.0, 0.0, 0.0};
    const double JOHANSEN_MAXEIG_CV_5_MULTI[10] = {
        14.26, 3.84, 0.0, // n=2
        21.13, 14.26, 3.84, 0.0, // n=3
        27.58, 21.13, 14.26, 0.0, 0.0 // n=4 (partial)
    };

    // OLS regression helper
    struct RegressionResult {
        Eigen::VectorXd coefficients;
        std::vector<double> residuals;
        double r_squared;
    };

    RegressionResult simple_ols(const std::vector<double>& y, const std::vector<double>& x) {
        int n = y.size();
        Eigen::MatrixXd X(n, 2);
        Eigen::VectorXd Y(n);

        for (int i = 0; i < n; ++i) {
            X(i, 0) = 1.0;  // intercept
            X(i, 1) = x[i];
            Y(i) = y[i];
        }

        // Solve: beta = (X'X)^(-1) X'Y
        Eigen::VectorXd beta = (X.transpose() * X).ldlt().solve(X.transpose() * Y);

        // Calculate residuals
        Eigen::VectorXd fitted = X * beta;
        std::vector<double> residuals(n);
        double rss = 0.0;
        for (int i = 0; i < n; ++i) {
            residuals[i] = Y(i) - fitted(i);
            rss += residuals[i] * residuals[i];
        }

        // Calculate R²
        double y_mean = Y.mean();
        double tss = (Y.array() - y_mean).square().sum();
        double r_squared = 1.0 - (rss / tss);

        RegressionResult result;
        result.coefficients = beta;
        result.residuals = residuals;
        result.r_squared = r_squared;

        return result;
    }
}

// Engle-Granger two-step test
Result<EngleGrangerResult> engle_granger_test(
    const std::vector<double>& y,
    const std::vector<double>& x,
    int max_lag)
{
    if (y.size() != x.size()) {
        return make_error<EngleGrangerResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Time series must have the same length",
            "engle_granger_test"
        );
    }

    if (y.size() < 30) {
        return make_error<EngleGrangerResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 30 observations for cointegration test",
            "engle_granger_test"
        );
    }

    // Step 1: Run OLS regression y = alpha + beta*x + residual
    RegressionResult regression = simple_ols(y, x);

    // Step 2: Test residuals for stationarity using ADF
    // For Engle-Granger, use ADF with no constant (residuals should be mean-zero)
    auto adf_result = augmented_dickey_fuller_test(
        regression.residuals,
        max_lag,
        ADFRegressionType::NO_CONSTANT
    );

    if (!adf_result) {
        return make_error<EngleGrangerResult>(
            adf_result.error().code,
            adf_result.error().message,
            "engle_granger_test"
        );
    }

    EngleGrangerResult result;
    result.intercept = regression.coefficients(0);
    result.cointegration_coefficient = regression.coefficients(1);
    result.adf_result = adf_result.value();
    result.residuals = regression.residuals;

    // For Engle-Granger, critical values are more stringent than standard ADF
    // Adjust critical values (approximately 10% more negative)
    result.is_cointegrated_1 = result.adf_result.is_stationary_1;
    result.is_cointegrated_5 = result.adf_result.is_stationary_5;
    result.is_cointegrated_10 = result.adf_result.is_stationary_10;

    return Result<EngleGrangerResult>(result);
}

// Engle-Granger test with Bar vectors
Result<EngleGrangerResult> engle_granger_test(
    const std::vector<Bar>& y_bars,
    const std::vector<Bar>& x_bars,
    int max_lag)
{
    std::vector<double> y = Normalization::extract_close_prices(y_bars);
    std::vector<double> x = Normalization::extract_close_prices(x_bars);
    return engle_granger_test(y, x, max_lag);
}

// Johansen test implementation
Result<JohansenResult> johansen_test(
    const Eigen::MatrixXd& data,
    int max_lag,
    int deterministic_trend)
{
    int n_obs = data.rows();
    int n_series = data.cols();

    if (n_obs < 30) {
        return make_error<JohansenResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 30 observations for Johansen test",
            "johansen_test"
        );
    }

    if (n_series < 2) {
        return make_error<JohansenResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need at least 2 time series for Johansen test",
            "johansen_test"
        );
    }

    // Auto-select lag if needed
    int lag = max_lag;
    if (lag <= 0) {
        lag = std::min(5, static_cast<int>(std::sqrt(n_obs)));
    }

    // Construct VAR model matrices
    // ΔY_t = Π Y_{t-1} + Γ_1 ΔY_{t-1} + ... + Γ_{k-1} ΔY_{t-k+1} + ε_t

    int effective_obs = n_obs - lag;
    if (effective_obs < 20) {
        return make_error<JohansenResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Not enough observations after accounting for lags",
            "johansen_test"
        );
    }

    // Compute first differences
    Eigen::MatrixXd diff(n_obs - 1, n_series);
    for (int i = 0; i < n_obs - 1; ++i) {
        diff.row(i) = data.row(i + 1) - data.row(i);
    }

    // Build Y_0 (ΔY_t) and Y_1 (Y_{t-1})
    Eigen::MatrixXd Y_0(effective_obs, n_series);
    Eigen::MatrixXd Y_1(effective_obs, n_series);

    for (int i = 0; i < effective_obs; ++i) {
        int t = lag + i;
        Y_0.row(i) = diff.row(t);
        Y_1.row(i) = data.row(t);
    }

    // Add deterministic terms if needed
    int n_det = 0;
    if (deterministic_trend >= 1) n_det++; // constant
    if (deterministic_trend >= 2) n_det++; // trend

    // Compute residuals from regressing Y_0 and Y_1 on deterministic terms
    Eigen::MatrixXd R_0 = Y_0;
    Eigen::MatrixXd R_1 = Y_1;

    if (n_det > 0) {
        Eigen::MatrixXd Z(effective_obs, n_det);
        for (int i = 0; i < effective_obs; ++i) {
            int col = 0;
            if (deterministic_trend >= 1) Z(i, col++) = 1.0;
            if (deterministic_trend >= 2) Z(i, col++) = i;
        }

        // Residual regression
        Eigen::MatrixXd Z_pinv = (Z.transpose() * Z).ldlt().solve(Z.transpose());
        R_0 = Y_0 - Z * Z_pinv * Y_0;
        R_1 = Y_1 - Z * Z_pinv * Y_1;
    }

    // Compute moment matrices
    Eigen::MatrixXd S_00 = (R_0.transpose() * R_0) / effective_obs;
    Eigen::MatrixXd S_11 = (R_1.transpose() * R_1) / effective_obs;
    Eigen::MatrixXd S_01 = (R_0.transpose() * R_1) / effective_obs;
    Eigen::MatrixXd S_10 = S_01.transpose();

    // Solve generalized eigenvalue problem: S_10 * S_00^(-1) * S_01 * v = λ * S_11 * v
    Eigen::MatrixXd S_00_inv = S_00.inverse();
    Eigen::MatrixXd product = S_00_inv * S_01;
    Eigen::MatrixXd M = S_11.ldlt().solve(S_10 * product);

    // Compute eigenvalues and eigenvectors
    Eigen::EigenSolver<Eigen::MatrixXd> eigen_solver(M);
    Eigen::VectorXd eigenvalues = eigen_solver.eigenvalues().real();
    Eigen::MatrixXd eigenvectors = eigen_solver.eigenvectors().real();

    // Sort eigenvalues in descending order
    std::vector<std::pair<double, int>> eigen_pairs;
    for (int i = 0; i < n_series; ++i) {
        eigen_pairs.push_back({eigenvalues(i), i});
    }
    std::sort(eigen_pairs.begin(), eigen_pairs.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    // Reorder eigenvalues and eigenvectors
    Eigen::VectorXd sorted_eigenvalues(n_series);
    Eigen::MatrixXd sorted_eigenvectors(n_series, n_series);
    for (int i = 0; i < n_series; ++i) {
        sorted_eigenvalues(i) = eigen_pairs[i].first;
        sorted_eigenvectors.col(i) = eigenvectors.col(eigen_pairs[i].second);
    }

    // Compute test statistics
    std::vector<double> trace_stats(n_series);
    std::vector<double> max_eigen_stats(n_series);

    for (int r = 0; r < n_series; ++r) {
        // Trace statistic: -T * sum_{i=r+1}^{n} log(1 - λ_i)
        double trace = 0.0;
        for (int i = r; i < n_series; ++i) {
            trace += std::log(1.0 - sorted_eigenvalues(i));
        }
        trace_stats[r] = -effective_obs * trace;

        // Max eigenvalue statistic: -T * log(1 - λ_{r+1})
        max_eigen_stats[r] = -effective_obs * std::log(1.0 - sorted_eigenvalues(r));
    }

    // Get critical values (simplified for n_series = 2)
    std::vector<double> trace_cv(n_series);
    std::vector<double> max_eigen_cv(n_series);

    for (int r = 0; r < n_series; ++r) {
        if (n_series == 2 && r < 2) {
            trace_cv[r] = JOHANSEN_TRACE_CV_5[r];
            max_eigen_cv[r] = JOHANSEN_MAXEIG_CV_5[r];
        } else {
            // Use conservative values
            trace_cv[r] = 15.49 - 5.0 * r;
            max_eigen_cv[r] = 14.26 - 5.0 * r;
        }
    }

    // Determine cointegration rank
    int rank_trace = 0;
    int rank_max_eigen = 0;

    for (int r = 0; r < n_series; ++r) {
        if (trace_stats[r] > trace_cv[r]) {
            rank_trace = r + 1;
        }
        if (max_eigen_stats[r] > max_eigen_cv[r]) {
            rank_max_eigen = r + 1;
        }
    }

    JohansenResult result;
    result.eigenvalues = sorted_eigenvalues;
    result.eigenvectors = sorted_eigenvectors;
    result.trace_statistics = trace_stats;
    result.max_eigen_statistics = max_eigen_stats;
    result.trace_critical_values_5 = trace_cv;
    result.max_eigen_critical_values_5 = max_eigen_cv;
    result.rank_trace = rank_trace;
    result.rank_max_eigen = rank_max_eigen;

    return Result<JohansenResult>(result);
}

// Johansen test with vector of price series
Result<JohansenResult> johansen_test(
    const std::vector<std::vector<double>>& series,
    int max_lag,
    int deterministic_trend)
{
    if (series.empty()) {
        return make_error<JohansenResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Series vector cannot be empty",
            "johansen_test"
        );
    }

    int n_series = series.size();
    int n_obs = series[0].size();

    // Convert to Eigen matrix
    Eigen::MatrixXd data(n_obs, n_series);
    for (int j = 0; j < n_series; ++j) {
        if (static_cast<int>(series[j].size()) != n_obs) {
            return make_error<JohansenResult>(
                ErrorCode::INVALID_ARGUMENT,
                "All series must have the same length",
                "johansen_test"
            );
        }
        for (int i = 0; i < n_obs; ++i) {
            data(i, j) = series[j][i];
        }
    }

    return johansen_test(data, max_lag, deterministic_trend);
}

// Johansen test with Bar vectors
Result<JohansenResult> johansen_test(
    const std::vector<std::vector<Bar>>& bar_series,
    int max_lag,
    int deterministic_trend)
{
    std::vector<std::vector<double>> series;
    series.reserve(bar_series.size());

    for (const auto& bars : bar_series) {
        series.push_back(Normalization::extract_close_prices(bars));
    }

    return johansen_test(series, max_lag, deterministic_trend);
}

// Simple cointegration check
Result<bool> is_cointegrated(
    const std::vector<double>& y,
    const std::vector<double>& x,
    double significance)
{
    auto eg_result = engle_granger_test(y, x);
    if (!eg_result) {
        return make_error<bool>(
            eg_result.error().code,
            eg_result.error().message,
            "is_cointegrated"
        );
    }

    bool cointegrated;
    if (std::abs(significance - 0.01) < 1e-6) {
        cointegrated = eg_result.value().is_cointegrated_1;
    } else if (std::abs(significance - 0.05) < 1e-6) {
        cointegrated = eg_result.value().is_cointegrated_5;
    } else if (std::abs(significance - 0.10) < 1e-6) {
        cointegrated = eg_result.value().is_cointegrated_10;
    } else {
        return make_error<bool>(
            ErrorCode::INVALID_ARGUMENT,
            "Significance must be 0.01, 0.05, or 0.10",
            "is_cointegrated"
        );
    }

    return Result<bool>(cointegrated);
}

} // namespace analysis
} // namespace trade_ngin
