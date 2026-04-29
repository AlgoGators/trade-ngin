#include "trade_ngin/statistics/state_estimation/dynamic_factor_model.hpp"
#include "trade_ngin/statistics/state_estimation/macro_data_loader.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <numeric>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

// ============================================================================
// Constructor
// ============================================================================

DynamicFactorModel::DynamicFactorModel(DFMConfig config)
    : config_(std::move(config)) {}

// ============================================================================
// fit()
// ============================================================================

Result<DFMOutput> DynamicFactorModel::fit(const Eigen::MatrixXd& data,
                                          const std::vector<std::string>& names)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const int T = static_cast<int>(data.rows());
    const int N = static_cast<int>(data.cols());
    const int K = config_.num_factors;

    DEBUG("[DFM::fit] entry: T=" << T << " N=" << N << " K=" << K);

    // ---- Validation -------------------------------------------------------
    if (T < 10) return make_error<DFMOutput>(ErrorCode::INVALID_ARGUMENT,
        "Need at least 10 rows", "DynamicFactorModel");
    if (N < 1) return make_error<DFMOutput>(ErrorCode::INVALID_ARGUMENT,
        "Need at least 1 column", "DynamicFactorModel");

    if (N < K) {
        return make_error<DFMOutput>(ErrorCode::INVALID_ARGUMENT,
            "num_factors (" + std::to_string(K) + ") must be <= num_series (" +
            std::to_string(N) + ")", "DynamicFactorModel");
    }

    // Per-column NaN check — all-NaN column is unusable
    for (int n = 0; n < N; ++n) {
        bool all_nan = true;
        for (int t = 0; t < T; ++t) {
            if (std::isfinite(data(t, n))) { all_nan = false; break; }
        }
        if (all_nan) {
            return make_error<DFMOutput>(ErrorCode::INVALID_ARGUMENT,
                "Column " + std::to_string(n) + " is entirely NaN/Inf",
                "DynamicFactorModel");
        }
    }

    // ---- Standardise ------------------------------------------------------
    Eigen::MatrixXd Y = config_.standardise_data
        ? standardise(data, data_mean_, data_std_)
        : data;

    if (!config_.standardise_data) {
        data_mean_ = Eigen::VectorXd::Zero(N);
        data_std_  = Eigen::VectorXd::Ones(N);
    }

    // ---- L-06: resolve factor sign anchors --------------------------------
    // Must happen before initialise_parameters so PCA sign-flip can use them.
    resolve_anchor_indices(names);

    // ---- Initialise parameters --------------------------------------------
    initialise_parameters(Y);

    // ---- EM loop ----------------------------------------------------------
    double prev_ll     = -std::numeric_limits<double>::infinity();
    double final_delta = 0.0;
    int    iter        = 0;
    bool   converged   = false;

    ConvergenceInfo conv_info;

    for (iter = 0; iter < config_.max_em_iterations; ++iter) {

        // E-step
        KalmanResult kr = kalman_filter_smoother(Y);
        conv_info.objective_history.push_back(kr.log_likelihood);

        // M-step sufficient statistics
        Eigen::MatrixXd S11 = Eigen::MatrixXd::Zero(K, K);
        Eigen::MatrixXd S10 = Eigen::MatrixXd::Zero(K, K);
        Eigen::MatrixXd S00 = Eigen::MatrixXd::Zero(K, K);

        for (int t = 0; t < T; ++t) {
            S11 += kr.smoothed_covs[t] +
                   kr.smoothed_means[t] * kr.smoothed_means[t].transpose();
        }
        for (int t = 1; t < T; ++t) {
            S10 += kr.cross_covs[t - 1] +
                   kr.smoothed_means[t] * kr.smoothed_means[t - 1].transpose();
            S00 += kr.smoothed_covs[t - 1] +
                   kr.smoothed_means[t - 1] * kr.smoothed_means[t - 1].transpose();
        }

        // Update A
        Eigen::MatrixXd S00_inv = S00.ldlt().solve(Eigen::MatrixXd::Identity(K, K));
        A_ = S10 * S00_inv;

        // Update Q
        Q_ = symmetrise((S11 - A_ * S10.transpose()) / static_cast<double>(T - 1));
        Q_ += Eigen::MatrixXd::Identity(K, K) * 1e-6;

        // Update Lambda and R (one series at a time)
        Eigen::MatrixXd S11_inv = S11.ldlt().solve(Eigen::MatrixXd::Identity(K, K));
        Eigen::MatrixXd new_lambda = Eigen::MatrixXd::Zero(N, K);
        Eigen::VectorXd new_R_diag = Eigen::VectorXd::Zero(N);

        for (int n = 0; n < N; ++n) {
            Eigen::VectorXd cross = Eigen::VectorXd::Zero(K);
            int T_valid = 0;
            for (int t = 0; t < T; ++t) {
                if (std::isfinite(Y(t, n))) {
                    cross += Y(t, n) * kr.smoothed_means[t];
                    ++T_valid;
                }
            }
            if (T_valid == 0) continue;

            new_lambda.row(n) = (S11_inv * cross).transpose();

            double r_sum = 0.0;
            for (int t = 0; t < T; ++t) {
                if (!std::isfinite(Y(t, n))) continue;
                double y_nt = Y(t, n);
                Eigen::MatrixXd Eff = kr.smoothed_covs[t] +
                    kr.smoothed_means[t] * kr.smoothed_means[t].transpose();
                r_sum += y_nt * y_nt
                       - 2.0 * new_lambda.row(n).dot(kr.smoothed_means[t]) * y_nt
                       + (new_lambda.row(n) * Eff * new_lambda.row(n).transpose())(0, 0);
            }
            new_R_diag(n) = std::max(r_sum / static_cast<double>(T_valid), 1e-4);
        }

        lambda_ = new_lambda;
        R_diag_ = new_R_diag;

        // Convergence check
        final_delta = std::abs(kr.log_likelihood - prev_ll);
        TRACE("[DFM::fit] iter=" << iter
              << " ll=" << kr.log_likelihood
              << " delta=" << final_delta);

        if (iter > 0 && final_delta < config_.em_tol) {
            converged = true;
            prev_ll   = kr.log_likelihood;
            break;
        }
        prev_ll = kr.log_likelihood;
    }

    conv_info.iterations        = iter;
    conv_info.converged         = converged;
    conv_info.final_tolerance   = final_delta;
    conv_info.termination_reason = converged ? "tolerance" : "max_iterations";

    if (!converged) {
        WARN("[DFM::fit] did not converge after " << iter
             << " iterations (final |ΔLL|=" << final_delta << ")");
    }

    // ---- Final smooth pass ------------------------------------------------
    KalmanResult final_kr = kalman_filter_smoother(Y);

    // Persist online Kalman state
    x_filt_ = final_kr.filtered_means.back();
    P_filt_ = final_kr.filtered_covs.back();
    fitted_  = true;

    // ---- Build output -----------------------------------------------------
    DFMOutput out;
    out.T = T; out.N = N; out.K = K;
    out.lambda    = lambda_;
    out.A         = A_;
    out.Q         = Q_;
    out.R_diag    = R_diag_;
    out.data_mean = data_mean_;
    out.data_std  = data_std_;
    out.log_likelihood  = prev_ll;
    out.em_iterations   = iter;
    out.converged       = converged;
    out.final_ll_delta  = final_delta;
    out.convergence_info = conv_info;

    out.series_names = names.empty()
        ? [&]{ std::vector<std::string> v(N);
               for (int n = 0; n < N; ++n) v[n] = "series_" + std::to_string(n);
               return v; }()
        : names;

    std::vector<std::string> labels = config_.factor_labels;
    if (static_cast<int>(labels.size()) != K) {
        labels.resize(K);
        for (int k = 0; k < K; ++k) labels[k] = "f" + std::to_string(k);
    }

    out.factors.resize(T, std::vector<double>(K));
    out.factor_uncertainty.resize(T, std::vector<double>(K));

    for (int t = 0; t < T; ++t) {
        for (int k = 0; k < K; ++k) {
            out.factors[t][k]            = final_kr.smoothed_means[t](k);
            out.factor_uncertainty[t][k] = std::sqrt(
                std::max(0.0, final_kr.smoothed_covs[t](k, k)));
        }
    }

    for (int k = 0; k < K; ++k) {
        std::vector<double> series(T);
        for (int t = 0; t < T; ++t) series[t] = final_kr.smoothed_means[t](k);
        out.factor_series[labels[k]] = std::move(series);
    }

    last_output_ = out;

    DEBUG("[DFM::fit] exit: converged=" << converged
          << " ll=" << prev_ll
          << " iters=" << iter);

    return Result<DFMOutput>(std::move(out));
}

// ============================================================================
// filter()  — forward-only, no lookahead
// ============================================================================

// filter() is STATELESS — runs a forward Kalman pass on `data` from
// scratch using the fitted Lambda/A/Q/R, returns the smoothed factors.
// It does NOT advance or modify the online x_filt_ / P_filt_ state.
//
// Contract:
//   - fit() sets x_filt_/P_filt_ to the panel-end Kalman state (line 178-179)
//   - filter(data) reads only the fitted parameters; does not touch x_filt_
//   - update(y_t) reads AND writes x_filt_/P_filt_ — advances online state
//
// Therefore, the call sequence `fit(panel) → filter(test) → update(new_obs)`
// runs `update()` from the END-OF-FIT state (NOT end-of-test). Callers
// who want online inference on an extended panel must use `update()`
// repeatedly; do not interleave `filter()` calls expecting them to advance
// the online state.
Result<DFMOutput> DynamicFactorModel::filter(const Eigen::MatrixXd& data) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<DFMOutput>(ErrorCode::NOT_INITIALIZED,
            "DFM has not been fitted — call fit() first",
            "DynamicFactorModel");
    }

    const int T = static_cast<int>(data.rows());
    const int N = static_cast<int>(data.cols());
    const int K = config_.num_factors;

    if (N != static_cast<int>(lambda_.rows())) {
        return make_error<DFMOutput>(ErrorCode::INVALID_ARGUMENT,
            "Data has " + std::to_string(N) + " columns but model fitted on " +
            std::to_string(lambda_.rows()), "DynamicFactorModel");
    }

    // Standardise with fitted stats
    Eigen::MatrixXd Y(T, N);
    for (int n = 0; n < N; ++n)
        Y.col(n) = (data.col(n).array() - data_mean_(n)) / data_std_(n);

    Eigen::VectorXd x = Eigen::VectorXd::Zero(K);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(K, K);

    DFMOutput out;
    out.T = T; out.N = N; out.K = K;
    out.lambda = lambda_; out.A = A_; out.Q = Q_; out.R_diag = R_diag_;
    out.data_mean = data_mean_; out.data_std = data_std_;
    out.factors.resize(T, std::vector<double>(K));
    out.factor_uncertainty.resize(T, std::vector<double>(K));

    for (int t = 0; t < T; ++t) {
        // Predict
        x = A_ * x;
        P = symmetrise(A_ * P * A_.transpose() + Q_);

        // Scalar update per observable
        for (int n = 0; n < N; ++n) {
            if (!std::isfinite(Y(t, n))) continue;
            Eigen::VectorXd h = lambda_.row(n).transpose();
            double S_n = (h.transpose() * P * h)(0) + R_diag_(n);
            if (S_n < 1e-12) continue;
            Eigen::VectorXd Kn = (P * h) / S_n;
            x = x + Kn * (Y(t, n) - h.dot(x));
            // Joseph form P = (I-KH) P (I-KH)' + K R K' preserves PD
            // under floating-point roundoff. Scalar update so K R K' = Kn Kn' * R_n.
            Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(K, K) - Kn * h.transpose();
            P = symmetrise(IKH * P * IKH.transpose() + R_diag_(n) * Kn * Kn.transpose());
        }

        for (int k = 0; k < K; ++k) {
            out.factors[t][k]            = x(k);
            out.factor_uncertainty[t][k] = std::sqrt(std::max(0.0, P(k, k)));
        }
    }

    std::vector<std::string> labels = config_.factor_labels;
    if (static_cast<int>(labels.size()) != K) {
        labels.resize(K);
        for (int k = 0; k < K; ++k) labels[k] = "f" + std::to_string(k);
    }
    for (int k = 0; k < K; ++k) {
        std::vector<double> series(T);
        for (int t = 0; t < T; ++t) series[t] = out.factors[t][k];
        out.factor_series[labels[k]] = std::move(series);
    }

    return Result<DFMOutput>(std::move(out));
}

// ============================================================================
// update()  — single-step online
// ============================================================================

Result<Eigen::VectorXd> DynamicFactorModel::update(const Eigen::VectorXd& y_t)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "DFM has not been fitted — call fit() first",
            "DynamicFactorModel");
    }

    const int K = config_.num_factors;
    const int N = static_cast<int>(lambda_.rows());

    if (y_t.size() != N) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_ARGUMENT,
            "Observation length " + std::to_string(y_t.size()) +
            " != model N=" + std::to_string(N), "DynamicFactorModel");
    }

    // Standardise
    Eigen::VectorXd y_std(N);
    for (int n = 0; n < N; ++n) {
        y_std(n) = std::isfinite(y_t(n))
            ? (y_t(n) - data_mean_(n)) / data_std_(n)
            : std::numeric_limits<double>::quiet_NaN();
    }

    // Predict
    x_filt_ = A_ * x_filt_;
    P_filt_ = symmetrise(A_ * P_filt_ * A_.transpose() + Q_);

    // Update
    for (int n = 0; n < N; ++n) {
        if (!std::isfinite(y_std(n))) continue;
        Eigen::VectorXd h = lambda_.row(n).transpose();
        double S_n = (h.transpose() * P_filt_ * h)(0) + R_diag_(n);
        if (S_n < 1e-12) continue;
        Eigen::VectorXd Kn = (P_filt_ * h) / S_n;
        x_filt_ = x_filt_ + Kn * (y_std(n) - h.dot(x_filt_));
        // Joseph form (online update path; long-running deployments
        // have many updates so PD preservation matters most here).
        Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(K, K) - Kn * h.transpose();
        P_filt_ = symmetrise(IKH * P_filt_ * IKH.transpose()
                             + R_diag_(n) * Kn * Kn.transpose());
    }

    return Result<Eigen::VectorXd>(x_filt_);
}

// ============================================================================
// kalman_filter_smoother()  — private
// ============================================================================

DynamicFactorModel::KalmanResult
DynamicFactorModel::kalman_filter_smoother(const Eigen::MatrixXd& Y) const
{
    const int T = static_cast<int>(Y.rows());
    const int N = static_cast<int>(Y.cols());
    const int K = config_.num_factors;

    KalmanResult kr;
    kr.filtered_means.resize(T);
    kr.filtered_covs.resize(T);
    kr.smoothed_means.resize(T);
    kr.smoothed_covs.resize(T);
    kr.cross_covs.resize(T - 1);

    // ---- Forward pass ----------------------------------------------------
    Eigen::VectorXd x = Eigen::VectorXd::Zero(K);
    Eigen::MatrixXd P = Eigen::MatrixXd::Identity(K, K) * 10.0;  // diffuse prior
    double log_lik = 0.0;

    for (int t = 0; t < T; ++t) {
        // Predict
        x = A_ * x;
        P = symmetrise(A_ * P * A_.transpose() + Q_);

        // Scalar update per observable
        for (int n = 0; n < N; ++n) {
            if (!std::isfinite(Y(t, n))) continue;
            Eigen::VectorXd h = lambda_.row(n).transpose();
            double S_n = (h.transpose() * P * h)(0) + R_diag_(n);
            if (S_n < 1e-12) continue;
            double innovation = Y(t, n) - h.dot(x);
            Eigen::VectorXd Kn = (P * h) / S_n;
            x = x + Kn * innovation;
            // Joseph form (Kalman filter+smoother E-step path)
            Eigen::MatrixXd IKH = Eigen::MatrixXd::Identity(K, K) - Kn * h.transpose();
            P = symmetrise(IKH * P * IKH.transpose() + R_diag_(n) * Kn * Kn.transpose());
            log_lik += -0.5 * (std::log(2.0 * M_PI * S_n) +
                               (innovation * innovation) / S_n);
        }

        kr.filtered_means[t] = x;
        kr.filtered_covs[t]  = P;
    }

    kr.log_likelihood = log_lik;

    // ---- RTS backward smoother ------------------------------------------
    kr.smoothed_means.back() = kr.filtered_means.back();
    kr.smoothed_covs.back()  = kr.filtered_covs.back();

    for (int t = T - 2; t >= 0; --t) {
        Eigen::MatrixXd P_pred = symmetrise(
            A_ * kr.filtered_covs[t] * A_.transpose() + Q_);

        // Smoother gain G_t = P_t^f * A' * P_{t+1|t}^{-1}
        Eigen::MatrixXd G = kr.filtered_covs[t] * A_.transpose() *
                            P_pred.ldlt().solve(Eigen::MatrixXd::Identity(K, K));

        kr.smoothed_means[t] = kr.filtered_means[t] +
            G * (kr.smoothed_means[t + 1] - A_ * kr.filtered_means[t]);

        kr.smoothed_covs[t] = symmetrise(
            kr.filtered_covs[t] +
            G * (kr.smoothed_covs[t + 1] - P_pred) * G.transpose());

        // Cross-covariance P_{t+1,t}^s = G_t * P_{t+1}^s  (P part only)
        kr.cross_covs[t] = kr.smoothed_covs[t + 1] * G.transpose();
    }

    return kr;
}

// ============================================================================
// initialise_parameters()  — private
// Warm-start Lambda via PCA of sample covariance, A=0.9I, Q=I, R=0.5
// ============================================================================

void DynamicFactorModel::initialise_parameters(const Eigen::MatrixXd& Y_std)
{
    const int T = static_cast<int>(Y_std.rows());
    const int N = static_cast<int>(Y_std.cols());
    const int K = config_.num_factors;

    // Pairwise complete-case covariance. Substituting NaN→0 then dividing
    // by (T-1) regardless of how many cells were observed biases the
    // covariance of series with many missing values toward zero, distorting
    // the Lambda init via PCA.
    //
    // For each pair (i, j), accumulate y_i*y_j only on rows where BOTH are
    // finite, and divide by (cnt_ij - 1). Diagonal uses cnt_ii (single-
    // column completeness). This is the standard pairwise estimator.
    Eigen::MatrixXd C = Eigen::MatrixXd::Zero(N, N);
    Eigen::MatrixXi cnt = Eigen::MatrixXi::Zero(N, N);
    for (int t = 0; t < T; ++t) {
        for (int i = 0; i < N; ++i) {
            if (!std::isfinite(Y_std(t, i))) continue;
            for (int j = i; j < N; ++j) {
                if (!std::isfinite(Y_std(t, j))) continue;
                C(i, j) += Y_std(t, i) * Y_std(t, j);
                cnt(i, j) += 1;
            }
        }
    }
    for (int i = 0; i < N; ++i) {
        for (int j = i; j < N; ++j) {
            if (cnt(i, j) > 1) {
                C(i, j) /= static_cast<double>(cnt(i, j) - 1);
            } else {
                C(i, j) = (i == j) ? 1.0 : 0.0;  // unobserved → identity prior
            }
            if (i != j) C(j, i) = C(i, j);  // symmetrise
        }
    }

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> solver(C);
    Eigen::MatrixXd evecs = solver.eigenvectors().rowwise().reverse();
    Eigen::VectorXd evals = solver.eigenvalues().reverse();

    lambda_ = Eigen::MatrixXd::Zero(N, K);
    for (int k = 0; k < K; ++k) {
        double scale = std::sqrt(std::max(evals(k), 1e-6));
        lambda_.col(k) = evecs.col(k) * scale;
    }

    // PCA eigenvectors have arbitrary sign. Lock factor orientation
    // to economic anchors so the macro pipeline's hardcoded sign-flips
    // remain stable across re-fits.
    for (int k = 0; k < K; ++k) {
        if (k >= static_cast<int>(anchor_indices_.size())) break;
        int anchor_idx = anchor_indices_[k];
        if (anchor_idx < 0 || anchor_idx >= N) continue;  // anchor not resolved
        int target_sign = (k < static_cast<int>(config_.factor_anchor_signs.size()))
            ? config_.factor_anchor_signs[k] : +1;
        if (target_sign == 0) continue;  // disabled
        double loading = lambda_(anchor_idx, k);
        if (loading * target_sign < 0) {
            lambda_.col(k) *= -1;
            DEBUG("[DFM::init] flipped factor " << k
                  << " sign to anchor on column " << anchor_idx
                  << " (loading was " << loading << ", target sign "
                  << target_sign << ")");
        }
    }

    A_      = Eigen::MatrixXd::Identity(K, K) * 0.9;
    Q_      = Eigen::MatrixXd::Identity(K, K);
    R_diag_ = Eigen::VectorXd::Constant(N, 0.5);
}

// Resolve factor_anchor_names against the input series_names vector.
// Stores indices in anchor_indices_ (length K). Missing anchors → -1.
void DynamicFactorModel::resolve_anchor_indices(const std::vector<std::string>& series_names) {
    const int K = config_.num_factors;
    anchor_indices_.assign(K, -1);

    if (config_.factor_anchor_names.empty()) {
        return;  // anchoring disabled — no-op
    }

    for (int k = 0; k < K; ++k) {
        if (k >= static_cast<int>(config_.factor_anchor_names.size())) break;
        const std::string& anchor = config_.factor_anchor_names[k];
        if (anchor.empty()) continue;

        bool found = false;
        for (int n = 0; n < static_cast<int>(series_names.size()); ++n) {
            if (series_names[n] == anchor) {
                anchor_indices_[k] = n;
                found = true;
                break;
            }
        }
        if (!found) {
            WARN("[DFM::init] factor " << k << " anchor '" << anchor
                 << "' not found in input series — sign-locking disabled for this factor");
        }
    }
}

// ============================================================================
// standardise()  — private
// ============================================================================

Eigen::MatrixXd DynamicFactorModel::standardise(const Eigen::MatrixXd& Y,
                                                 Eigen::VectorXd& out_mean,
                                                 Eigen::VectorXd& out_std) const
{
    const int T = static_cast<int>(Y.rows());
    const int N = static_cast<int>(Y.cols());

    out_mean = Eigen::VectorXd::Zero(N);
    out_std  = Eigen::VectorXd::Ones(N);

    for (int n = 0; n < N; ++n) {
        double sum = 0.0; int cnt = 0;
        for (int t = 0; t < T; ++t)
            if (std::isfinite(Y(t, n))) { sum += Y(t, n); ++cnt; }
        if (cnt > 0) out_mean(n) = sum / cnt;

        double ssq = 0.0; cnt = 0;
        for (int t = 0; t < T; ++t) {
            if (std::isfinite(Y(t, n))) {
                double d = Y(t, n) - out_mean(n);
                ssq += d * d; ++cnt;
            }
        }
        if (cnt > 1) {
            double s = std::sqrt(ssq / (cnt - 1));
            out_std(n) = s > 1e-10 ? s : 1.0;
        }
    }

    Eigen::MatrixXd Y_std(T, N);
    for (int n = 0; n < N; ++n)
        for (int t = 0; t < T; ++t)
            Y_std(t, n) = std::isfinite(Y(t, n))
                ? (Y(t, n) - out_mean(n)) / out_std(n)
                : std::numeric_limits<double>::quiet_NaN();

    return Y_std;
}

// ============================================================================
// fit_from_db()  — convenience: load macro panel from PostgreSQL and fit
// ============================================================================

Result<DFMOutput> DynamicFactorModel::fit_from_db(PostgresDatabase& db,
                                                   const std::string& start_date,
                                                   const std::string& end_date)
{
    MacroDataLoader loader(db);
    auto panel_result = loader.load(start_date, end_date);
    if (panel_result.is_error()) {
        return make_error<DFMOutput>(panel_result.error()->code(),
            std::string(panel_result.error()->what()),
            "DynamicFactorModel");
    }

    auto& panel = panel_result.value();
    INFO("[DFM::fit_from_db] loaded panel T=" << panel.T
         << " N=" << panel.N
         << " date_range=[" << panel.dates.front()
         << " .. " << panel.dates.back() << "]");

    return fit(panel.data, panel.column_names);
}

} // namespace statistics
} // namespace trade_ngin

// ============================================================================
// Factor Label Rationale
//
// The 3-factor DFM fitted on the macro_data panel (24 series, 2011–2026)
// extracts the following latent factors. Labels are based on observed
// loading patterns:
//
// Factor 0 — "macro_level"
//   The dominant factor, capturing the shared secular trend across nearly
//   all macro variables. High positive loadings on:
//     GDP (+0.85), CPI (+0.85), retail sales (+0.85), core CPI (+0.84),
//     core PCE (+0.84), nonfarm payrolls (+0.80), M2 (+0.78),
//     treasury 2y (+0.77), fed funds (+0.75), fed balance sheet (+0.68),
//     treasury 10y (+0.67), DXY (+0.62), breakeven 5y (+0.62)
//   Negative loadings on yield spread (-0.62), credit spreads (-0.58),
//   HY spread (-0.57), TED spread (-0.56), unemployment (-0.58).
//
//   Interpretation: this is the overall level of the economy — when it's
//   high, output/prices/rates/money supply are all elevated and spreads
//   are tight. It's the first principal component and explains the most
//   variance. Not labelled "growth" because it loads equally on nominal
//   variables (CPI, rates) as on real ones (GDP, payrolls).
//
// Factor 1 — "real_activity"
//   Captures the cyclical state of the real economy, orthogonal to the
//   broad level. Key loadings:
//     capacity utilization (-0.60), industrial production (-0.52),
//     unemployment (+0.40), VIX (+0.31), WTI crude (-0.33)
//   Negative on capacity util / IP means: when this factor is HIGH,
//   factories are running below capacity, output is falling, unemployment
//   is rising, and volatility is elevated — i.e., cyclical weakness.
//   When LOW, the real economy is running hot.
//
// Factor 2 — "commodity_inflation"
//   Captures inflation expectations and commodity-driven price pressure,
//   orthogonal to the first two factors. Key loadings:
//     WTI crude (-0.42), breakeven 5y (-0.29), unemployment (-0.26),
//     capacity utilization (+0.28), TED spread (+0.22)
//   When this factor is HIGH: oil prices are low, inflation expectations
//   are subdued, and interbank stress (TED) is slightly elevated — a
//   disinflationary or deflationary signal. When LOW: oil rallying,
//   breakevens rising, inflation pressure building.
//
// Note: PCA-based initialisation does not guarantee stable factor ordering
// across re-estimations. If the data panel changes materially (e.g., new
// series added), re-inspect the loadings and relabel as needed.
// ============================================================================
