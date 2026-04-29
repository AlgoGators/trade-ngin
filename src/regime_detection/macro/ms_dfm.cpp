#include "trade_ngin/statistics/state_estimation/ms_dfm.hpp"
#include "trade_ngin/statistics/state_estimation/macro_data_loader.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/core/logger.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <limits>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

// ============================================================================
// Constructor
// ============================================================================

MarkovSwitchingDFM::MarkovSwitchingDFM(MSDFMConfig config)
    : config_(std::move(config)) {}

// ============================================================================
// mvn_log_prob  — multivariate Gaussian log-density (static)
// ============================================================================

double MarkovSwitchingDFM::mvn_log_prob(const Eigen::VectorXd& x,
                                         const Eigen::VectorXd& mean,
                                         const Eigen::MatrixXd& cov)
{
    const int K = static_cast<int>(x.size());
    Eigen::VectorXd diff = x - mean;

    // Cholesky decomposition for numerical stability
    Eigen::LLT<Eigen::MatrixXd> llt(cov);
    if (llt.info() != Eigen::Success) {
        // Fallback: add jitter and retry
        Eigen::MatrixXd cov_jitter = cov + Eigen::MatrixXd::Identity(K, K) * 1e-4;
        Eigen::LLT<Eigen::MatrixXd> llt2(cov_jitter);
        if (llt2.info() != Eigen::Success) {
            return -1e10;  // degenerate — return very low log-prob
        }
        Eigen::VectorXd solved = llt2.solve(diff);
        double log_det = 0.0;
        for (int i = 0; i < K; ++i)
            log_det += 2.0 * std::log(llt2.matrixL()(i, i));
        return -0.5 * (K * std::log(2.0 * M_PI) + log_det + diff.dot(solved));
    }

    Eigen::VectorXd solved = llt.solve(diff);
    double log_det = 0.0;
    for (int i = 0; i < K; ++i)
        log_det += 2.0 * std::log(llt.matrixL()(i, i));

    return -0.5 * (K * std::log(2.0 * M_PI) + log_det + diff.dot(solved));
}

// ============================================================================
// ergodic_distribution  — stationary distribution of transition matrix (static)
// ============================================================================

Eigen::VectorXd MarkovSwitchingDFM::ergodic_distribution(const Eigen::MatrixXd& P)
{
    const int J = static_cast<int>(P.rows());

    // Solve pi = P' * pi, sum(pi) = 1
    // Equivalent to finding left eigenvector of P with eigenvalue 1
    // Use: (P' - I) * pi = 0, with constraint sum(pi) = 1
    // Replace last row with constraint
    Eigen::MatrixXd A = P.transpose() - Eigen::MatrixXd::Identity(J, J);
    A.row(J - 1).setOnes();

    Eigen::VectorXd b = Eigen::VectorXd::Zero(J);
    b(J - 1) = 1.0;

    Eigen::VectorXd pi = A.colPivHouseholderQr().solve(b);

    // Ensure non-negative and normalized
    for (int j = 0; j < J; ++j)
        pi(j) = std::max(pi(j), 0.0);
    double sum = pi.sum();
    if (sum > 0) pi /= sum;
    else pi = Eigen::VectorXd::Ones(J) / J;

    return pi;
}

// ============================================================================
// initialize_parameters
// ============================================================================

void MarkovSwitchingDFM::initialize_parameters(const Eigen::MatrixXd& factors)
{
    const int T = static_cast<int>(factors.rows());
    K_ = static_cast<int>(factors.cols());
    J_ = config_.n_regimes;

    // Fit global OLS VAR(1):  f_t = A * f_{t-1} + residual
    Eigen::MatrixXd X = factors.topRows(T - 1);    // (T-1) x K
    Eigen::MatrixXd Y = factors.bottomRows(T - 1); // (T-1) x K

    // A_global = (X'X + ridge)^{-1} X'Y
    Eigen::MatrixXd XtX = X.transpose() * X;
    XtX += Eigen::MatrixXd::Identity(K_, K_) * 1e-6;  // ridge
    Eigen::MatrixXd A_global = XtX.ldlt().solve(X.transpose() * Y);
    A_global.transposeInPlace();  // K x K

    // Residual covariance
    Eigen::MatrixXd residuals = Y - X * A_global.transpose();
    Eigen::MatrixXd Q_global = symmetrise(
        (residuals.transpose() * residuals) / static_cast<double>(T - 2));
    Q_global += Eigen::MatrixXd::Identity(K_, K_) * 1e-6;

    // Perturb per regime to break symmetry
    A_.resize(J_);
    Q_.resize(J_);
    for (int j = 0; j < J_; ++j) {
        double a_scale = 1.0 - 0.05 * (j - (J_ - 1) / 2.0);
        A_[j] = A_global * a_scale;

        double q_scale = 0.5 + static_cast<double>(j);  // regime 0=calm, J-1=volatile
        Q_[j] = Q_global * q_scale;
        Q_[j] += Eigen::MatrixXd::Identity(K_, K_) * 1e-6;
    }

    // Transition matrix: high diagonal persistence
    double off_diag = (1.0 - config_.transition_persistence) / (J_ - 1);
    P_ = Eigen::MatrixXd::Constant(J_, J_, off_diag);
    for (int j = 0; j < J_; ++j)
        P_(j, j) = config_.transition_persistence;

    // Initial regime probs: uniform
    pi0_ = Eigen::VectorXd::Ones(J_) / J_;
}

// ============================================================================
// hamilton_filter_step  — forward pass
// ============================================================================

double MarkovSwitchingDFM::hamilton_filter_step(const Eigen::MatrixXd& factors,
                                                 Eigen::MatrixXd& filtered_probs)
{
    const int T = static_cast<int>(factors.rows());

    filtered_probs = Eigen::MatrixXd::Zero(T, J_);
    pred_probs_ = Eigen::MatrixXd::Zero(T, J_);

    // t = 0: initial probs, no emission (no f_{-1})
    filtered_probs.row(0) = pi0_.transpose();
    pred_probs_.row(0) = pi0_.transpose();

    double total_ll = 0.0;

    for (int t = 1; t < T; ++t) {
        // Predicted regime probs: P(Z_t=j | y_{1:t-1})
        Eigen::VectorXd pred = P_.transpose() * filtered_probs.row(t - 1).transpose();
        pred_probs_.row(t) = pred.transpose();

        // Emission log-likelihoods per regime
        Eigen::VectorXd log_emit(J_);
        for (int j = 0; j < J_; ++j) {
            Eigen::VectorXd mu_j = A_[j] * factors.row(t - 1).transpose();
            log_emit(j) = mvn_log_prob(factors.row(t).transpose(), mu_j, Q_[j]);
        }

        // Joint: log P(Z_t=j, f_t | f_{1:t-1})
        Eigen::VectorXd log_joint(J_);
        for (int j = 0; j < J_; ++j) {
            log_joint(j) = log_emit(j) + std::log(std::max(pred(j), 1e-300));
        }

        // Log-sum-exp for normalization
        double log_marginal = critical_values::log_sum_exp(log_joint.data(), J_);
        total_ll += log_marginal;

        // Filtered probs
        for (int j = 0; j < J_; ++j) {
            filtered_probs(t, j) = std::exp(log_joint(j) - log_marginal);
        }
    }

    return total_ll;
}

// ============================================================================
// kim_smoother_step  — backward pass
// Same pattern as MarkovSwitching::kim_smoother
// ============================================================================

void MarkovSwitchingDFM::kim_smoother_step(const Eigen::MatrixXd& filtered_probs,
                                            Eigen::MatrixXd& smoothed_probs)
{
    const int T = static_cast<int>(filtered_probs.rows());

    smoothed_probs = Eigen::MatrixXd::Zero(T, J_);
    smoothed_probs.row(T - 1) = filtered_probs.row(T - 1);

    for (int t = T - 2; t >= 0; --t) {
        Eigen::VectorXd pred = pred_probs_.row(t + 1).transpose();

        for (int i = 0; i < J_; ++i) {
            double sum = 0.0;
            for (int j = 0; j < J_; ++j) {
                double pred_j = std::max(pred(j), 1e-300);
                sum += P_(i, j) * smoothed_probs(t + 1, j) / pred_j;
            }
            smoothed_probs(t, i) = filtered_probs(t, i) * sum;
        }

        // Normalize
        double row_sum = smoothed_probs.row(t).sum();
        if (row_sum > 0) {
            smoothed_probs.row(t) /= row_sum;
        }
    }
}

// ============================================================================
// m_step  — update A_j, Q_j, P
// ============================================================================

void MarkovSwitchingDFM::m_step(const Eigen::MatrixXd& factors,
                                 const Eigen::MatrixXd& smoothed_probs,
                                 const Eigen::MatrixXd& filtered_probs)
{
    const int T = static_cast<int>(factors.rows());

    // Update A_j, Q_j per regime
    for (int j = 0; j < J_; ++j) {
        Eigen::MatrixXd S_ff = Eigen::MatrixXd::Zero(K_, K_);
        Eigen::MatrixXd S_yf = Eigen::MatrixXd::Zero(K_, K_);
        double gamma_sum = 0.0;

        for (int t = 1; t < T; ++t) {
            double w = smoothed_probs(t, j);
            Eigen::VectorXd f_prev = factors.row(t - 1).transpose();
            Eigen::VectorXd f_curr = factors.row(t).transpose();
            S_ff += w * f_prev * f_prev.transpose();
            S_yf += w * f_curr * f_prev.transpose();
            gamma_sum += w;
        }

        // Guard against degenerate regime
        if (gamma_sum < 1e-8) {
            WARN("[MS-DFM] regime " << j << " has near-zero occupation, skipping M-step");
            continue;
        }

        // A_j = S_yf * S_ff^{-1}  (with ridge)
        S_ff += Eigen::MatrixXd::Identity(K_, K_) * 1e-6;
        A_[j] = S_yf * S_ff.ldlt().solve(Eigen::MatrixXd::Identity(K_, K_));

        // Q_j = weighted residual covariance
        Eigen::MatrixXd S_qq = Eigen::MatrixXd::Zero(K_, K_);
        for (int t = 1; t < T; ++t) {
            double w = smoothed_probs(t, j);
            Eigen::VectorXd resid = factors.row(t).transpose() -
                                    A_[j] * factors.row(t - 1).transpose();
            S_qq += w * resid * resid.transpose();
        }
        Q_[j] = symmetrise(S_qq / gamma_sum);
        Q_[j] += Eigen::MatrixXd::Identity(K_, K_) * 1e-6;  // PD guarantee
    }

    // Update transition matrix P
    for (int i = 0; i < J_; ++i) {
        double from_i_sum = 0.0;
        for (int t = 0; t < T - 1; ++t) {
            from_i_sum += smoothed_probs(t, i);
        }

        if (from_i_sum > 1e-10) {
            for (int j = 0; j < J_; ++j) {
                double xi_sum = 0.0;
                for (int t = 0; t < T - 1; ++t) {
                    double pred_j = std::max(pred_probs_(t + 1, j), 1e-300);
                    xi_sum += smoothed_probs(t, i) * P_(i, j) *
                              smoothed_probs(t + 1, j) / pred_j;
                }
                P_(i, j) = std::max(xi_sum / from_i_sum, 1e-6);
            }
            // Normalize row
            double row_sum = P_.row(i).sum();
            P_.row(i) /= row_sum;
        }
    }

    // Update initial probs
    pi0_ = smoothed_probs.row(0).transpose();
}

// ============================================================================
// order_regimes_by_volatility  — sort so regime 0 = calm, J-1 = stress
// ============================================================================

void MarkovSwitchingDFM::order_regimes_by_volatility(MSDFMOutput& out)
{
    // Compute volatility per regime
    std::vector<std::pair<double, int>> vol_idx(J_);
    for (int j = 0; j < J_; ++j) {
        vol_idx[j] = {Q_[j].trace() / K_, j};
    }
    std::sort(vol_idx.begin(), vol_idx.end());

    // Build permutation
    std::vector<int> perm(J_);
    for (int j = 0; j < J_; ++j) {
        perm[vol_idx[j].second] = j;
    }

    // Check if already sorted
    bool is_sorted = true;
    for (int j = 0; j < J_; ++j) {
        if (perm[j] != j) { is_sorted = false; break; }
    }
    if (is_sorted) return;

    // Permute A_, Q_
    auto old_A = A_;
    auto old_Q = Q_;
    for (int j = 0; j < J_; ++j) {
        A_[perm[j]] = old_A[j];
        Q_[perm[j]] = old_Q[j];
    }

    // Permute transition matrix
    Eigen::MatrixXd old_P = P_;
    for (int i = 0; i < J_; ++i)
        for (int j = 0; j < J_; ++j)
            P_(perm[i], perm[j]) = old_P(i, j);

    // Permute pi0
    Eigen::VectorXd old_pi0 = pi0_;
    for (int j = 0; j < J_; ++j)
        pi0_(perm[j]) = old_pi0(j);

    // Permute output columns
    Eigen::MatrixXd old_filt = out.filtered_probs;
    Eigen::MatrixXd old_smooth = out.smoothed_probs;
    for (int j = 0; j < J_; ++j) {
        out.filtered_probs.col(perm[j]) = old_filt.col(j);
        out.smoothed_probs.col(perm[j]) = old_smooth.col(j);
    }

    // Permute regime signatures
    auto old_sigs = out.regime_signatures;
    for (int j = 0; j < J_; ++j) {
        out.regime_signatures[perm[j]] = old_sigs[j];
    }

    // Permute regime_labels alongside the other state-indexed arrays. A_,
    // Q_, P_, pi0_, filtered/smoothed probs, signatures, and decoded states
    // are all reordered to match the calm→stress sort; without the same
    // permutation here, a caller that supplied human labels (e.g.,
    // ["calm", "transitional", "stress"]) would end up with sorted state
    // index 0 still labeled "calm" but the underlying state j=0 could be
    // a different one. The pipeline doesn't read regime_labels (it indexes
    // into smoothed_probs), so this is a diagnostic-only concern and
    // cannot shift any belief output.
    if (!out.regime_labels.empty()) {
        auto old_labels = out.regime_labels;
        for (int j = 0; j < J_; ++j) {
            out.regime_labels[perm[j]] = old_labels[j];
        }
    }

    // Re-decode
    for (int t = 0; t < out.T; ++t) {
        int max_idx;
        out.smoothed_probs.row(t).maxCoeff(&max_idx);
        out.decoded_regimes[t] = max_idx;
    }

    // Permute transition matrix in output
    out.transition_matrix = P_;

    // Recompute ergodic
    out.ergodic_probs = ergodic_distribution(P_);
}

// ============================================================================
// fit(MatrixXd)  — core EM on factor matrix
// ============================================================================

Result<MSDFMOutput> MarkovSwitchingDFM::fit(const Eigen::MatrixXd& factors)
{
    std::lock_guard<std::mutex> lock(mutex_);

    const int T = static_cast<int>(factors.rows());
    const int K = static_cast<int>(factors.cols());

    DEBUG("[MS-DFM::fit] entry: T=" << T << " K=" << K
          << " J=" << config_.n_regimes);

    // Validation
    if (T < 20) return make_error<MSDFMOutput>(ErrorCode::INVALID_ARGUMENT,
        "Need at least 20 rows", "MarkovSwitchingDFM");
    if (K < 1) return make_error<MSDFMOutput>(ErrorCode::INVALID_ARGUMENT,
        "Need at least 1 factor", "MarkovSwitchingDFM");
    if (config_.n_regimes < 2) return make_error<MSDFMOutput>(ErrorCode::INVALID_ARGUMENT,
        "Need at least 2 regimes", "MarkovSwitchingDFM");

    // Check for finite data
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < K; ++k)
            if (!std::isfinite(factors(t, k)))
                return make_error<MSDFMOutput>(ErrorCode::INVALID_ARGUMENT,
                    "Non-finite value at t=" + std::to_string(t) +
                    " k=" + std::to_string(k), "MarkovSwitchingDFM");

    // Initialize
    initialize_parameters(factors);

    // EM loop
    double prev_ll = -std::numeric_limits<double>::infinity();
    double final_delta = 0.0;
    int iter = 0;
    bool converged = false;

    ConvergenceInfo conv_info;

    Eigen::MatrixXd filtered_probs, smoothed_probs;

    for (iter = 0; iter < config_.max_em_iterations; ++iter) {

        // E-step
        double ll = hamilton_filter_step(factors, filtered_probs);
        kim_smoother_step(filtered_probs, smoothed_probs);

        conv_info.objective_history.push_back(ll);

        // Convergence check
        final_delta = std::abs(ll - prev_ll);
        TRACE("[MS-DFM::fit] iter=" << iter << " ll=" << ll
              << " delta=" << final_delta);

        if (iter > 0 && final_delta < config_.em_tol) {
            converged = true;
            prev_ll = ll;
            break;
        }
        prev_ll = ll;

        // M-step
        m_step(factors, smoothed_probs, filtered_probs);
    }

    conv_info.iterations = iter;
    conv_info.converged = converged;
    conv_info.final_tolerance = final_delta;
    conv_info.termination_reason = converged ? "tolerance" : "max_iterations";

    if (!converged) {
        WARN("[MS-DFM::fit] did not converge after " << iter
             << " iterations (final |ΔLL|=" << final_delta << ")");
    }

    // Build output
    MSDFMOutput out;
    out.T = T;
    out.K = K;
    out.J = J_;
    out.filtered_probs = filtered_probs;
    out.smoothed_probs = smoothed_probs;
    out.transition_matrix = P_;
    out.log_likelihood = prev_ll;
    out.convergence_info = conv_info;

    // Regime labels
    out.regime_labels = config_.regime_labels;
    if (static_cast<int>(out.regime_labels.size()) != J_) {
        out.regime_labels.resize(J_);
        for (int j = 0; j < J_; ++j)
            out.regime_labels[j] = "regime_" + std::to_string(j);
    }

    // Regime signatures
    out.regime_signatures.resize(J_);
    for (int j = 0; j < J_; ++j) {
        out.regime_signatures[j].A = A_[j];
        out.regime_signatures[j].Q = Q_[j];
        out.regime_signatures[j].mean_volatility = Q_[j].trace() / K;

        // Weighted mean factor level
        Eigen::VectorXd mean_f = Eigen::VectorXd::Zero(K);
        double gamma_sum = 0.0;
        for (int t = 0; t < T; ++t) {
            mean_f += smoothed_probs(t, j) * factors.row(t).transpose();
            gamma_sum += smoothed_probs(t, j);
        }
        if (gamma_sum > 0) mean_f /= gamma_sum;
        out.regime_signatures[j].mean_factors = mean_f;
    }

    // Decoded regimes
    out.decoded_regimes.resize(T);
    for (int t = 0; t < T; ++t) {
        int max_idx;
        smoothed_probs.row(t).maxCoeff(&max_idx);
        out.decoded_regimes[t] = max_idx;
    }

    // Order regimes by volatility
    order_regimes_by_volatility(out);

    // Ergodic distribution
    out.ergodic_probs = ergodic_distribution(P_);

    // Warn about degenerate regimes (< 1% ergodic occupation)
    for (int j = 0; j < J_; ++j) {
        if (out.ergodic_probs(j) < 0.01) {
            WARN("[MS-DFM] regime '" << out.regime_labels[j]
                 << "' has <1% ergodic probability ("
                 << out.ergodic_probs(j) << ") — effectively unused");
        }
    }

    // Set up online state
    online_regime_probs_ = filtered_probs.row(T - 1).transpose();
    f_prev_ = factors.row(T - 1).transpose();
    fitted_ = true;
    last_output_ = out;

    DEBUG("[MS-DFM::fit] exit: converged=" << converged
          << " ll=" << prev_ll << " iters=" << iter);

    return Result<MSDFMOutput>(std::move(out));
}

// ============================================================================
// fit(DFMOutput)  — convenience
// ============================================================================

Result<MSDFMOutput> MarkovSwitchingDFM::fit(const DFMOutput& dfm_output)
{
    const int T = dfm_output.T;
    const int K = dfm_output.K;

    // Extract factor matrix from DFMOutput
    Eigen::MatrixXd factors(T, K);
    for (int t = 0; t < T; ++t)
        for (int k = 0; k < K; ++k)
            factors(t, k) = dfm_output.factors[t][k];

    return fit(factors);
}

// ============================================================================
// fit_from_db  — full pipeline: load → DFM → MS-DFM
// ============================================================================

Result<MSDFMOutput> MarkovSwitchingDFM::fit_from_db(PostgresDatabase& db,
                                                     const std::string& start_date,
                                                     const std::string& end_date)
{
    // Fit DFM first
    DynamicFactorModel dfm(config_.dfm_config);
    auto dfm_result = dfm.fit_from_db(db, start_date, end_date);
    if (dfm_result.is_error()) {
        return make_error<MSDFMOutput>(dfm_result.error()->code(),
            "DFM fit failed: " + std::string(dfm_result.error()->what()),
            "MarkovSwitchingDFM");
    }

    INFO("[MS-DFM::fit_from_db] DFM fit complete, fitting MS-DFM on "
         << dfm_result.value().K << " factors");

    return fit(dfm_result.value());
}

// ============================================================================
// update  — online regime probability update
// ============================================================================

Result<Eigen::VectorXd> MarkovSwitchingDFM::update(const Eigen::VectorXd& f_t)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!fitted_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "MS-DFM has not been fitted — call fit() first",
            "MarkovSwitchingDFM");
    }

    if (f_t.size() != K_) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_ARGUMENT,
            "Factor dimension " + std::to_string(f_t.size()) +
            " != model K=" + std::to_string(K_), "MarkovSwitchingDFM");
    }

    // Predict regime probs
    Eigen::VectorXd pred = P_.transpose() * online_regime_probs_;

    // Emission log-likelihoods
    Eigen::VectorXd log_emit(J_);
    for (int j = 0; j < J_; ++j) {
        Eigen::VectorXd mu_j = A_[j] * f_prev_;
        log_emit(j) = mvn_log_prob(f_t, mu_j, Q_[j]);
    }

    // Joint log-probability
    Eigen::VectorXd log_joint(J_);
    for (int j = 0; j < J_; ++j) {
        log_joint(j) = log_emit(j) + std::log(std::max(pred(j), 1e-300));
    }

    // Normalize
    double log_marginal = critical_values::log_sum_exp(log_joint.data(), J_);
    for (int j = 0; j < J_; ++j) {
        online_regime_probs_(j) = std::exp(log_joint(j) - log_marginal);
    }

    f_prev_ = f_t;

    return Result<Eigen::VectorXd>(online_regime_probs_);
}

} // namespace statistics
} // namespace trade_ngin
