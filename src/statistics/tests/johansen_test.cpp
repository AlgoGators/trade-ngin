#include "trade_ngin/statistics/tests/johansen_test.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <algorithm>

namespace trade_ngin {
namespace statistics {

JohansenTest::JohansenTest(JohansenTestConfig config)
    : config_(config) {}

Result<CointegrationResult> JohansenTest::test(const Eigen::MatrixXd& data) const {
    {
        auto valid = validation::validate_matrix(data, 20, 2, "JohansenTest");
        if (valid.is_error()) {
            return make_error<CointegrationResult>(valid.error()->code(), valid.error()->what(), "JohansenTest");
        }
    }

    int n = data.rows();
    DEBUG("[JohansenTest::test] entry: rows=" << n << " cols=" << data.cols()
          << " max_lags=" << config_.max_lags);
    int p = data.cols();
    int k = config_.max_lags;

    // Difference the data
    Eigen::MatrixXd dY = data.bottomRows(n - 1) - data.topRows(n - 1);
    Eigen::MatrixXd Y_lagged = data.topRows(n - k - 1);
    Eigen::MatrixXd dY_current = dY.bottomRows(n - k - 1);

    int n_obs = n - k - 1;

    // Residuals from regressing dY on lagged differences (if k > 0)
    Eigen::MatrixXd R0 = dY_current;
    Eigen::MatrixXd R1 = Y_lagged;

    if (k > 0) {
        Eigen::MatrixXd dY_lagged(n_obs, p * k);
        for (int lag = 0; lag < k; ++lag) {
            dY_lagged.block(0, lag * p, n_obs, p) =
                dY.block(k - lag - 1, 0, n_obs, p);
        }

        // Residuals from regressions
        Eigen::MatrixXd M0 = (dY_lagged.transpose() * dY_lagged).ldlt().solve(
            dY_lagged.transpose() * dY_current);
        R0 = dY_current - dY_lagged * M0;

        Eigen::MatrixXd M1 = (dY_lagged.transpose() * dY_lagged).ldlt().solve(
            dY_lagged.transpose() * Y_lagged);
        R1 = Y_lagged - dY_lagged * M1;
    }

    // Product moment matrices
    Eigen::MatrixXd S00 = (R0.transpose() * R0) / n_obs;
    Eigen::MatrixXd S11 = (R1.transpose() * R1) / n_obs;
    Eigen::MatrixXd S01 = (R0.transpose() * R1) / n_obs;
    Eigen::MatrixXd S10 = S01.transpose();

    // Solve eigenvalue problem: S11^{-1} S10 S00^{-1} S01
    Eigen::MatrixXd M = S11.ldlt().solve(S10) * S00.ldlt().solve(S01);

    Eigen::EigenSolver<Eigen::MatrixXd> solver(M);
    if (solver.info() != Eigen::Success) {
        return make_error<CointegrationResult>(
            ErrorCode::INVALID_DATA,
            "Eigenvalue decomposition failed in Johansen test",
            "JohansenTest"
        );
    }

    Eigen::VectorXcd eigenvalues_complex = solver.eigenvalues();
    Eigen::MatrixXcd eigenvectors_complex = solver.eigenvectors();

    // Extract real eigenvalues and sort
    std::vector<std::pair<double, int>> eig_pairs;
    for (int i = 0; i < p; ++i) {
        eig_pairs.push_back({eigenvalues_complex(i).real(), i});
    }
    std::sort(eig_pairs.begin(), eig_pairs.end(),
             [](const auto& a, const auto& b) { return a.first > b.first; });

    CointegrationResult result;
    result.eigenvalues.resize(p);
    result.cointegrating_vectors = Eigen::MatrixXd(p, p);

    for (int i = 0; i < p; ++i) {
        result.eigenvalues[i] = eig_pairs[i].first;
        int idx = eig_pairs[i].second;
        result.cointegrating_vectors.col(i) = eigenvectors_complex.col(idx).real();
    }

    // Calculate trace statistics
    result.trace_statistics.resize(p);
    for (int r = 0; r < p; ++r) {
        double trace = 0.0;
        for (int i = r; i < p; ++i) {
            trace += std::log(1.0 - result.eigenvalues[i]);
        }
        result.trace_statistics[r] = -n_obs * trace;
    }

    // Critical values (approximate for p=2, can be extended)
    result.critical_values = get_critical_values(p, 0);

    // Determine cointegration rank
    result.cointegration_rank = 0;
    result.is_cointegrated = false;
    for (int r = 0; r < p; ++r) {
        if (result.trace_statistics[r] > result.critical_values[r]) {
            result.cointegration_rank = r + 1;
            result.is_cointegrated = true;
        } else {
            break;
        }
    }

    DEBUG("[JohansenTest::test] exit: cointegration_rank=" << result.cointegration_rank);

    return Result<CointegrationResult>(std::move(result));
}

std::vector<double> JohansenTest::get_critical_values(int n_series, int rank) const {
    return critical_values::johansen_trace_critical_values(n_series, config_.significance_level);
}

} // namespace statistics
} // namespace trade_ngin
