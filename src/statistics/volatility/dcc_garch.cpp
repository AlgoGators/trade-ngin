#include "trade_ngin/statistics/volatility/dcc_garch.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <nlopt.hpp>
#include <cmath>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

DCCGARCH::DCCGARCH(DCCGARCHConfig config)
    : config_(config)
    , dcc_a_(config.dcc_a)
    , dcc_b_(config.dcc_b) {}

struct DCCOptData {
    const Eigen::MatrixXd* std_residuals;
    const Eigen::MatrixXd* Q_bar;
    int n_series;
};

static double dcc_nlopt_objective(const std::vector<double>& x, std::vector<double>&, void* data) {
    auto* opt_data = static_cast<DCCOptData*>(data);
    double a = x[0];
    double b = x[1];

    if (a + b >= 0.999 || a < 0 || b < 0) {
        return std::numeric_limits<double>::infinity();
    }

    const auto& eps = *opt_data->std_residuals;
    const auto& Q_bar = *opt_data->Q_bar;
    int T = eps.rows();
    int K = opt_data->n_series;
    (void)K;

    Eigen::MatrixXd Q = Q_bar;
    double ll = 0.0;

    for (int t = 0; t < T; ++t) {
        if (t > 0) {
            Eigen::VectorXd e_prev = eps.row(t - 1).transpose();
            Q = (1.0 - a - b) * Q_bar + a * (e_prev * e_prev.transpose()) + b * Q;
        }

        // R_t = diag(Q_t)^{-1/2} * Q_t * diag(Q_t)^{-1/2}
        Eigen::VectorXd q_diag = Q.diagonal().array().sqrt().inverse();
        Eigen::MatrixXd R = q_diag.asDiagonal() * Q * q_diag.asDiagonal();

        // Ensure R is valid
        double det = R.determinant();
        if (det <= 0 || !std::isfinite(det)) {
            return std::numeric_limits<double>::infinity();
        }

        Eigen::VectorXd e_t = eps.row(t).transpose();
        Eigen::LDLT<Eigen::MatrixXd> ldlt(R);
        Eigen::VectorXd solved = ldlt.solve(e_t);

        ll += -0.5 * (std::log(det) + e_t.dot(solved) - e_t.squaredNorm());
    }

    if (!std::isfinite(ll)) return std::numeric_limits<double>::infinity();
    return -ll;
}

Result<DCCGARCHResult> DCCGARCH::fit(const Eigen::MatrixXd& returns) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(returns, 50, 2, "DCCGARCH");
        if (valid.is_error()) {
            return make_error<DCCGARCHResult>(valid.error()->code(), valid.error()->what(), "DCCGARCH");
        }
    }

    int T = returns.rows();
    n_series_ = returns.cols();

    DEBUG("[DCCGARCH::fit] entry: T=" << T << " K=" << n_series_);

    // Stage 1: Fit univariate GARCH to each series
    Eigen::MatrixXd std_residuals(T, n_series_);
    Eigen::VectorXd last_volatilities(n_series_);

    for (int k = 0; k < n_series_; ++k) {
        GARCH garch(config_.univariate_config);
        std::vector<double> series(T);
        for (int t = 0; t < T; ++t) series[t] = returns(t, k);

        auto fit_result = garch.fit(series);
        if (fit_result.is_error()) {
            return make_error<DCCGARCHResult>(fit_result.error()->code(),
                "Univariate GARCH fit failed for series " + std::to_string(k), "DCCGARCH");
        }

        // Compute standardized residuals: ε_t / σ_t
        double mean_r = 0.0;
        for (double v : series) mean_r += v;
        mean_r /= T;

        // Reconstruct conditional variances
        double omega = garch.get_omega();
        double alpha = garch.get_alpha();
        double beta = garch.get_beta();

        double var = 0.0;
        for (double v : series) var += (v - mean_r) * (v - mean_r);
        var /= T;

        std::vector<double> cond_var(T, var);
        for (int t = 1; t < T; ++t) {
            double resid = series[t - 1] - mean_r;
            cond_var[t] = omega + alpha * resid * resid + beta * cond_var[t - 1];
            if (cond_var[t] <= 0) cond_var[t] = var;
        }

        for (int t = 0; t < T; ++t) {
            double resid = series[t] - mean_r;
            std_residuals(t, k) = resid / std::sqrt(cond_var[t]);
        }

        auto vol = garch.get_current_volatility();
        last_volatilities(k) = vol.is_ok() ? vol.value() : std::sqrt(var);
    }

    // Stage 2: Estimate DCC parameters
    // Unconditional correlation of standardized residuals
    Q_bar_ = (std_residuals.transpose() * std_residuals) / T;

    estimate_dcc_params(std_residuals);

    // Compute conditional correlations
    result_.dcc_a = dcc_a_;
    result_.dcc_b = dcc_b_;
    result_.unconditional_correlation = Q_bar_;
    result_.conditional_correlations.resize(T);
    result_.conditional_volatilities.resize(T);

    Eigen::MatrixXd Q = Q_bar_;
    last_Q_ = Q_bar_;

    for (int t = 0; t < T; ++t) {
        if (t > 0) {
            Eigen::VectorXd e_prev = std_residuals.row(t - 1).transpose();
            Q = (1.0 - dcc_a_ - dcc_b_) * Q_bar_ + dcc_a_ * (e_prev * e_prev.transpose()) + dcc_b_ * Q;
        }

        Eigen::VectorXd q_diag = Q.diagonal().array().sqrt().inverse();
        Eigen::MatrixXd R = q_diag.asDiagonal() * Q * q_diag.asDiagonal();

        result_.conditional_correlations[t] = R;
        last_Q_ = Q;

        result_.conditional_volatilities[t] = last_volatilities;
    }

    last_std_resid_ = std_residuals.row(T - 1).transpose();
    fitted_ = true;

    DEBUG("[DCCGARCH::fit] exit: a=" << dcc_a_ << " b=" << dcc_b_);

    return Result<DCCGARCHResult>(result_);
}

void DCCGARCH::estimate_dcc_params(const Eigen::MatrixXd& std_residuals) {
    // Grid search + nlopt for DCC parameters
    double best_ll = -std::numeric_limits<double>::infinity();
    dcc_a_ = config_.dcc_a;
    dcc_b_ = config_.dcc_b;

    for (double a = 0.01; a <= 0.2; a += 0.03) {
        for (double b = 0.7; b <= 0.98; b += 0.04) {
            if (a + b < 0.999) {
                double ll = dcc_log_likelihood(std_residuals, a, b);
                if (ll > best_ll) {
                    best_ll = ll;
                    dcc_a_ = a;
                    dcc_b_ = b;
                }
            }
        }
    }

    // Refine with nlopt
    try {
        nlopt::opt opt(nlopt::LN_BOBYQA, 2);
        opt.set_lower_bounds({1e-4, 0.01});
        opt.set_upper_bounds({0.5, 0.999});

        DCCOptData opt_data{&std_residuals, &Q_bar_, n_series_};
        opt.set_min_objective(dcc_nlopt_objective, &opt_data);
        opt.set_ftol_rel(1e-8);
        opt.set_maxeval(300);

        std::vector<double> x = {dcc_a_, dcc_b_};
        double min_obj;
        opt.optimize(x, min_obj);

        double refined_ll = -min_obj;
        if (refined_ll > best_ll && x[0] + x[1] < 0.999) {
            dcc_a_ = x[0];
            dcc_b_ = x[1];
        }
    } catch (const std::exception& e) {
        DEBUG("[DCCGARCH::estimate_dcc_params] nlopt failed: " << e.what());
    }
}

double DCCGARCH::dcc_log_likelihood(const Eigen::MatrixXd& std_residuals,
                                     double a, double b) const {
    int T = std_residuals.rows();
    Eigen::MatrixXd Q = Q_bar_;
    double ll = 0.0;

    for (int t = 0; t < T; ++t) {
        if (t > 0) {
            Eigen::VectorXd e_prev = std_residuals.row(t - 1).transpose();
            Q = (1.0 - a - b) * Q_bar_ + a * (e_prev * e_prev.transpose()) + b * Q;
        }

        Eigen::VectorXd q_diag = Q.diagonal().array().sqrt().inverse();
        Eigen::MatrixXd R = q_diag.asDiagonal() * Q * q_diag.asDiagonal();

        double det = R.determinant();
        if (det <= 0 || !std::isfinite(det)) return -std::numeric_limits<double>::infinity();

        Eigen::VectorXd e_t = std_residuals.row(t).transpose();
        Eigen::LDLT<Eigen::MatrixXd> ldlt(R);
        Eigen::VectorXd solved = ldlt.solve(e_t);

        ll += -0.5 * (std::log(det) + e_t.dot(solved) - e_t.squaredNorm());
    }

    return std::isfinite(ll) ? ll : -std::numeric_limits<double>::infinity();
}

Result<Eigen::MatrixXd> DCCGARCH::get_correlation(int t) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED, "DCC-GARCH has not been fitted", "DCCGARCH");
    }

    if (t < 0 || t >= static_cast<int>(result_.conditional_correlations.size())) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::INVALID_ARGUMENT, "Time index out of range", "DCCGARCH");
    }

    return Result<Eigen::MatrixXd>(result_.conditional_correlations[t]);
}

Result<Eigen::MatrixXd> DCCGARCH::forecast_correlation() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::NOT_INITIALIZED, "DCC-GARCH has not been fitted", "DCCGARCH");
    }

    // Q_{t+1} = (1-a-b)*Q̄ + a*ε_t*ε'_t + b*Q_t
    Eigen::MatrixXd Q_next = (1.0 - dcc_a_ - dcc_b_) * Q_bar_
                             + dcc_a_ * (last_std_resid_ * last_std_resid_.transpose())
                             + dcc_b_ * last_Q_;

    Eigen::VectorXd q_diag = Q_next.diagonal().array().sqrt().inverse();
    Eigen::MatrixXd R_next = q_diag.asDiagonal() * Q_next * q_diag.asDiagonal();

    return Result<Eigen::MatrixXd>(std::move(R_next));
}

} // namespace statistics
} // namespace trade_ngin
