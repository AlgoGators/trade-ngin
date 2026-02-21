#include "trade_ngin/statistics/regression/ols_regression.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

OLSRegression::OLSRegression(OLSRegressionConfig config)
    : config_(config) {}

Eigen::MatrixXd OLSRegression::prepare_X(const Eigen::MatrixXd& X) const {
    if (!config_.include_intercept) return X;
    Eigen::MatrixXd Xa(X.rows(), X.cols() + 1);
    Xa.col(0) = Eigen::VectorXd::Ones(X.rows());
    Xa.rightCols(X.cols()) = X;
    return Xa;
}

Result<OLSResult> OLSRegression::fit(const Eigen::MatrixXd& X, const Eigen::VectorXd& y) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(X, 2, 1, "OLSRegression");
        if (valid.is_error()) return make_error<OLSResult>(valid.error()->code(), valid.error()->what(), "OLSRegression");
    }

    if (X.rows() != y.size()) {
        return make_error<OLSResult>(ErrorCode::INVALID_ARGUMENT,
            "X rows (" + std::to_string(X.rows()) + ") must match y size (" + std::to_string(y.size()) + ")",
            "OLSRegression");
    }

    if (!y.allFinite()) {
        return make_error<OLSResult>(ErrorCode::INVALID_DATA, "NaN/Inf detected in y", "OLSRegression");
    }

    Eigen::MatrixXd Xa = prepare_X(X);
    int n = Xa.rows();
    int p = Xa.cols();

    if (n <= p) {
        return make_error<OLSResult>(ErrorCode::INVALID_ARGUMENT,
            "Need more observations than parameters (n=" + std::to_string(n) + ", p=" + std::to_string(p) + ")",
            "OLSRegression");
    }

    DEBUG("[OLSRegression::fit] n=" << n << " p=" << p);

    // Solve via ColPivHouseholderQR for numerical stability
    Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(Xa);
    Eigen::VectorXd beta = qr.solve(y);

    // Residuals and R²
    Eigen::VectorXd residuals = y - Xa * beta;
    double ss_res = residuals.squaredNorm();
    double y_mean = y.mean();
    double ss_tot = (y.array() - y_mean).matrix().squaredNorm();

    double r_squared = (ss_tot > 0) ? 1.0 - ss_res / ss_tot : 1.0;
    int df_res = n - p;
    double adj_r_squared = 1.0 - (1.0 - r_squared) * (n - 1.0) / df_res;

    // Standard errors, t-stats, p-values
    double mse = ss_res / df_res;
    Eigen::MatrixXd XtX_inv = (Xa.transpose() * Xa).inverse();
    Eigen::VectorXd se = (mse * XtX_inv.diagonal()).array().sqrt();
    Eigen::VectorXd t_stats = beta.array() / se.array();

    // p-values using normal CDF approximation via erfc
    Eigen::VectorXd p_vals(p);
    for (int i = 0; i < p; ++i) {
        p_vals(i) = std::erfc(std::abs(t_stats(i)) / std::sqrt(2.0));
    }

    result_.coefficients = beta;
    result_.r_squared = r_squared;
    result_.adj_r_squared = adj_r_squared;
    result_.residuals = residuals;
    result_.standard_errors = se;
    result_.t_statistics = t_stats;
    result_.p_values = p_vals;
    fitted_ = true;

    return Result<OLSResult>(result_);
}

Result<Eigen::VectorXd> OLSRegression::predict(const Eigen::MatrixXd& X) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "OLS model has not been fitted", "OLSRegression");
    }

    Eigen::MatrixXd Xa = prepare_X(X);
    if (Xa.cols() != result_.coefficients.size()) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_ARGUMENT,
            "Feature dimension mismatch", "OLSRegression");
    }

    Eigen::VectorXd predictions = Xa * result_.coefficients;
    return Result<Eigen::VectorXd>(std::move(predictions));
}

} // namespace statistics
} // namespace trade_ngin
