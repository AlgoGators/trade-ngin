#include "trade_ngin/statistics/state_estimation/hmm.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <limits>

// Define M_PI if not available (Windows)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

HMM::HMM(HMMConfig config)
    : config_(config) {}

Result<void> HMM::initialize(const Eigen::VectorXd& initial_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initial_state.size() != config_.n_states) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Initial state dimension must match number of states",
            "HMM"
        );
    }

    if (!initial_state.allFinite()) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "NaN/Inf detected in initial state",
            "HMM"
        );
    }

    state_probs_ = initial_state;
    initial_probs_ = initial_state;

    // Initialize default emission parameters if not already set
    if (means_.empty()) {
        means_.resize(config_.n_states);
        covariances_.resize(config_.n_states);

        // Simple default initialization: states at different locations
        for (int k = 0; k < config_.n_states; ++k) {
            means_[k] = Eigen::VectorXd::Zero(1);  // Default to 1D
            means_[k](0) = static_cast<double>(k);
            covariances_[k] = Eigen::MatrixXd::Identity(1, 1);
        }
    }

    // Initialize default transition matrix if not set
    if (transition_matrix_.size() == 0) {
        transition_matrix_ = Eigen::MatrixXd::Ones(config_.n_states, config_.n_states) / config_.n_states;
    }

    initialized_ = true;

    return Result<void>();
}

Result<void> HMM::fit(const Eigen::MatrixXd& observations) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_matrix(observations, 10, 1, "HMM");
        if (valid.is_error()) return valid;
    }

    int T = observations.rows();
    int D = observations.cols();

    DEBUG("[HMM::fit] entry: T=" << T << " D=" << D << " n_states=" << config_.n_states);

    // Initialize parameters
    initialize_parameters(observations);

    // Baum-Welch algorithm (EM)
    double prev_log_likelihood = -std::numeric_limits<double>::infinity();
    bool converged = false;

    for (int iter = 0; iter < config_.max_iterations; ++iter) {
        Eigen::MatrixXd gamma(T, config_.n_states);
        Eigen::MatrixXd xi(T - 1, config_.n_states * config_.n_states);

        // E-step: Forward-backward algorithm
        double log_likelihood = forward_backward(observations, gamma, xi);

        double delta = std::abs(log_likelihood - prev_log_likelihood);
        TRACE("[HMM::fit] iter=" << iter << " ll=" << log_likelihood << " delta=" << delta);

        // Check convergence
        if (delta < config_.tolerance) {
            converged = true;
            break;
        }
        prev_log_likelihood = log_likelihood;

        // M-step: Update parameters

        // Update initial probabilities
        initial_probs_ = gamma.row(0).transpose();

        // Update transition matrix
        for (int i = 0; i < config_.n_states; ++i) {
            double row_sum = gamma.col(i).segment(0, T - 1).sum();
            for (int j = 0; j < config_.n_states; ++j) {
                double xi_sum = 0.0;
                for (int t = 0; t < T - 1; ++t) {
                    xi_sum += xi(t, i * config_.n_states + j);
                }
                transition_matrix_(i, j) = xi_sum / row_sum;
            }
        }

        // Update emission parameters (Gaussian means and covariances)
        for (int k = 0; k < config_.n_states; ++k) {
            double gamma_sum = gamma.col(k).sum();

            // Update mean
            means_[k] = Eigen::VectorXd::Zero(D);
            for (int t = 0; t < T; ++t) {
                means_[k] += gamma(t, k) * observations.row(t).transpose();
            }
            means_[k] /= gamma_sum;

            // Update covariance
            covariances_[k] = Eigen::MatrixXd::Zero(D, D);
            for (int t = 0; t < T; ++t) {
                Eigen::VectorXd diff = observations.row(t).transpose() - means_[k];
                covariances_[k] += gamma(t, k) * (diff * diff.transpose());
            }
            covariances_[k] /= gamma_sum;

            // Check for near-singular covariance before regularization
            Eigen::LLT<Eigen::MatrixXd> llt_check(covariances_[k]);
            if (llt_check.info() != Eigen::Success) {
                WARN("[HMM::fit] near-singular covariance for state " << k << " at iter " << iter);
            }

            // Add small regularization to prevent singularity
            covariances_[k] += Eigen::MatrixXd::Identity(D, D) * 1e-6;
        }
    }

    if (!converged) {
        WARN("[HMM::fit] did not converge after " << config_.max_iterations << " iterations");
    }

    initialized_ = true;
    DEBUG("[HMM::fit] exit: converged=" << converged);
    return Result<void>();
}

void HMM::initialize_parameters(const Eigen::MatrixXd& observations) {
    int D = observations.cols();

    // Initialize state probabilities uniformly
    initial_probs_ = Eigen::VectorXd::Ones(config_.n_states) / config_.n_states;
    state_probs_ = initial_probs_;

    // Initialize transition matrix
    if (config_.init_random) {
        transition_matrix_ = Eigen::MatrixXd::Random(config_.n_states, config_.n_states).array().abs();
        // Normalize rows to sum to 1
        for (int i = 0; i < config_.n_states; ++i) {
            double row_sum = transition_matrix_.row(i).sum();
            transition_matrix_.row(i) /= row_sum;
        }
    } else {
        transition_matrix_ = Eigen::MatrixXd::Ones(config_.n_states, config_.n_states) / config_.n_states;
    }

    // Initialize emission parameters using k-means-like approach
    means_.resize(config_.n_states);
    covariances_.resize(config_.n_states);

    int step = observations.rows() / config_.n_states;
    for (int k = 0; k < config_.n_states; ++k) {
        int idx = k * step;
        means_[k] = observations.row(idx).transpose();
        covariances_[k] = Eigen::MatrixXd::Identity(D, D);
    }
}

double HMM::forward_backward(const Eigen::MatrixXd& observations,
                            Eigen::MatrixXd& gamma,
                            Eigen::MatrixXd& xi) const {
    int T = observations.rows();
    int N = config_.n_states;

    // Pre-compute log quantities
    Eigen::MatrixXd log_emit(T, N);
    for (int t = 0; t < T; ++t) {
        for (int j = 0; j < N; ++j) {
            log_emit(t, j) = log_emission_probability(observations.row(t), j);
        }
    }

    Eigen::MatrixXd log_A(N, N);
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            log_A(i, j) = (transition_matrix_(i, j) > 0)
                ? std::log(transition_matrix_(i, j))
                : -std::numeric_limits<double>::infinity();
        }
    }

    Eigen::VectorXd log_pi(N);
    for (int i = 0; i < N; ++i) {
        log_pi(i) = (initial_probs_(i) > 0)
            ? std::log(initial_probs_(i))
            : -std::numeric_limits<double>::infinity();
    }

    // Log forward pass
    Eigen::MatrixXd log_alpha(T, N);
    for (int i = 0; i < N; ++i) {
        log_alpha(0, i) = log_pi(i) + log_emit(0, i);
    }

    std::vector<double> temp(N);
    for (int t = 1; t < T; ++t) {
        for (int j = 0; j < N; ++j) {
            for (int i = 0; i < N; ++i) {
                temp[i] = log_alpha(t - 1, i) + log_A(i, j);
            }
            log_alpha(t, j) = critical_values::log_sum_exp(temp.data(), N) + log_emit(t, j);
        }
    }

    // Log backward pass
    Eigen::MatrixXd log_beta(T, N);
    log_beta.row(T - 1).setZero();  // log(1) = 0

    for (int t = T - 2; t >= 0; --t) {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                temp[j] = log_A(i, j) + log_emit(t + 1, j) + log_beta(t + 1, j);
            }
            log_beta(t, i) = critical_values::log_sum_exp(temp.data(), N);
        }
    }

    // Compute gamma: convert back to probability space for M-step
    for (int t = 0; t < T; ++t) {
        // log_norm for this time step
        for (int i = 0; i < N; ++i) {
            temp[i] = log_alpha(t, i) + log_beta(t, i);
        }
        double log_norm = critical_values::log_sum_exp(temp.data(), N);
        for (int i = 0; i < N; ++i) {
            gamma(t, i) = std::exp(log_alpha(t, i) + log_beta(t, i) - log_norm);
        }
    }

    // Compute xi: transition posterior
    std::vector<double> temp_xi(N * N);
    for (int t = 0; t < T - 1; ++t) {
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                temp_xi[i * N + j] = log_alpha(t, i) + log_A(i, j) +
                                     log_emit(t + 1, j) + log_beta(t + 1, j);
            }
        }
        double log_norm = critical_values::log_sum_exp(temp_xi.data(), N * N);
        for (int i = 0; i < N; ++i) {
            for (int j = 0; j < N; ++j) {
                xi(t, i * N + j) = std::exp(temp_xi[i * N + j] - log_norm);
            }
        }
    }

    // Return log-likelihood from forward variables
    for (int i = 0; i < N; ++i) {
        temp[i] = log_alpha(T - 1, i);
    }
    return critical_values::log_sum_exp(temp.data(), N);
}

double HMM::log_emission_probability(const Eigen::VectorXd& obs, int state) const {
    Eigen::Index D = obs.size();
    Eigen::VectorXd diff = obs - means_[state];

    // Use LLT Cholesky for numerical stability
    Eigen::LLT<Eigen::MatrixXd> llt(covariances_[state]);
    if (llt.info() == Eigen::Success) {
        // Mahalanobis distance via triangular solve
        Eigen::VectorXd v = llt.matrixL().solve(diff);
        double mahal_sq = v.squaredNorm();
        // Log-determinant = 2 * sum(log(diag(L)))
        double log_det = 2.0 * llt.matrixL().toDenseMatrix().diagonal().array().log().sum();
        return -0.5 * (D * std::log(2.0 * M_PI) + log_det + mahal_sq);
    } else {
        // LDLT fallback
        Eigen::LDLT<Eigen::MatrixXd> ldlt(covariances_[state]);
        Eigen::VectorXd solved = ldlt.solve(diff);
        double mahal_sq = diff.dot(solved);
        // Log-determinant from LDLT: sum of log of |D diagonal|
        double log_det = ldlt.vectorD().array().abs().log().sum();
        return -0.5 * (D * std::log(2.0 * M_PI) + log_det + mahal_sq);
    }
}

double HMM::emission_probability(const Eigen::VectorXd& obs, int state) const {
    return std::exp(log_emission_probability(obs, state));
}

Result<std::vector<int>> HMM::decode(const Eigen::MatrixXd& observations) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<std::vector<int>>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been fitted or initialized",
            "HMM"
        );
    }

    int T = observations.rows();

    // Viterbi algorithm
    Eigen::MatrixXd delta(T, config_.n_states);
    Eigen::MatrixXi psi(T, config_.n_states);

    // Initialize
    for (int i = 0; i < config_.n_states; ++i) {
        double log_pi = (initial_probs_(i) > 0)
            ? std::log(initial_probs_(i))
            : -std::numeric_limits<double>::infinity();
        delta(0, i) = log_pi + log_emission_probability(observations.row(0), i);
        psi(0, i) = 0;
    }

    // Recursion
    for (int t = 1; t < T; ++t) {
        for (int j = 0; j < config_.n_states; ++j) {
            double max_val = -std::numeric_limits<double>::infinity();
            int max_state = 0;

            for (int i = 0; i < config_.n_states; ++i) {
                double log_a = (transition_matrix_(i, j) > 0)
                    ? std::log(transition_matrix_(i, j))
                    : -std::numeric_limits<double>::infinity();
                double val = delta(t - 1, i) + log_a;
                if (val > max_val) {
                    max_val = val;
                    max_state = i;
                }
            }

            delta(t, j) = max_val + log_emission_probability(observations.row(t), j);
            psi(t, j) = max_state;
        }
    }

    // Backtrack
    std::vector<int> states(T);

    // Find most likely final state
    int max_idx;
    delta.row(T - 1).maxCoeff(&max_idx);
    states[T - 1] = max_idx;

    // Backtrack through time
    for (int t = T - 2; t >= 0; --t) {
        states[t] = psi(t + 1, states[t + 1]);
    }

    return Result<std::vector<int>>(std::move(states));
}

Result<Eigen::VectorXd> HMM::predict() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been initialized",
            "HMM"
        );
    }

    // Predict next state probabilities
    Eigen::VectorXd next_probs = transition_matrix_.transpose() * state_probs_;
    return Result<Eigen::VectorXd>(next_probs);
}

Result<Eigen::VectorXd> HMM::update(const Eigen::VectorXd& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been initialized",
            "HMM"
        );
    }

    // Predict
    Eigen::VectorXd predicted_probs = transition_matrix_.transpose() * state_probs_;

    // Update with observation likelihood
    for (int i = 0; i < config_.n_states; ++i) {
        predicted_probs(i) *= emission_probability(observation, i);
    }

    // Normalize
    double sum = predicted_probs.sum();
    if (sum > 0) {
        state_probs_ = predicted_probs / sum;
    }

    return Result<Eigen::VectorXd>(state_probs_);
}

Result<Eigen::VectorXd> HMM::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "HMM has not been initialized",
            "HMM"
        );
    }

    return Result<Eigen::VectorXd>(state_probs_);
}

} // namespace statistics
} // namespace trade_ngin
