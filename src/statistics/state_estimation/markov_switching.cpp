#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/statistics/validation.hpp"
#include "trade_ngin/statistics/critical_values.hpp"
#include "trade_ngin/core/logger.hpp"
#include <cmath>
#include <limits>
#include <algorithm>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace trade_ngin {
namespace statistics {

MarkovSwitching::MarkovSwitching(MarkovSwitchingConfig config)
    : config_(config) {}

double MarkovSwitching::emission_log_prob(double obs, int state) const {
    double mean = state_means_(state);
    double var = state_variances_(state);
    double diff = obs - mean;
    return -0.5 * (std::log(2.0 * M_PI) + std::log(var) + diff * diff / var);
}

void MarkovSwitching::initialize_parameters(const std::vector<double>& data) {
    int K = config_.n_states;

    // Sort data to find quantile-based initialization
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());

    state_means_ = Eigen::VectorXd(K);
    state_variances_ = Eigen::VectorXd(K);

    for (int k = 0; k < K; ++k) {
        int idx = static_cast<int>((k + 0.5) * sorted.size() / K);
        idx = std::min(idx, static_cast<int>(sorted.size()) - 1);
        state_means_(k) = sorted[idx];
    }

    // Initial variance: overall variance divided by n_states
    double mean = 0.0;
    for (double v : data) mean += v;
    mean /= data.size();

    double var = 0.0;
    for (double v : data) var += (v - mean) * (v - mean);
    var /= data.size();

    for (int k = 0; k < K; ++k) {
        state_variances_(k) = var + 1e-6;
    }

    // Transition matrix: diagonal = 0.95
    transition_matrix_ = Eigen::MatrixXd::Constant(K, K, 0.05 / (K - 1));
    for (int k = 0; k < K; ++k) {
        transition_matrix_(k, k) = 0.95;
    }

    // Initial state probabilities: uniform
    state_probs_ = Eigen::VectorXd::Ones(K) / K;
}

double MarkovSwitching::hamilton_filter(const std::vector<double>& data,
                                         Eigen::MatrixXd& filtered_probs) const {
    int T = static_cast<int>(data.size());
    int K = config_.n_states;

    filtered_probs = Eigen::MatrixXd::Zero(T, K);

    // Initial predicted probabilities
    Eigen::VectorXd pred_probs = state_probs_;
    double total_ll = 0.0;

    for (int t = 0; t < T; ++t) {
        // Compute emission log-probabilities for this observation
        Eigen::VectorXd log_emit(K);
        for (int k = 0; k < K; ++k) {
            log_emit(k) = emission_log_prob(data[t], k);
        }

        // Joint probability: P(S_t = k, y_t | y_{1:t-1}) = P(y_t | S_t=k) * P(S_t=k | y_{1:t-1})
        // Work in log space for stability
        Eigen::VectorXd log_joint(K);
        for (int k = 0; k < K; ++k) {
            log_joint(k) = log_emit(k) + std::log(std::max(pred_probs(k), 1e-300));
        }

        // Log-sum-exp for normalization
        double log_marginal = critical_values::log_sum_exp(log_joint.data(), K);
        total_ll += log_marginal;

        // Filtered probabilities: P(S_t = k | y_{1:t})
        for (int k = 0; k < K; ++k) {
            filtered_probs(t, k) = std::exp(log_joint(k) - log_marginal);
        }

        // Prediction for next time step: P(S_{t+1} = k | y_{1:t})
        if (t < T - 1) {
            pred_probs = transition_matrix_.transpose() * filtered_probs.row(t).transpose();
        }
    }

    return total_ll;
}

void MarkovSwitching::kim_smoother(const Eigen::MatrixXd& filtered_probs,
                                    Eigen::MatrixXd& smoothed_probs) const {
    int T = filtered_probs.rows();
    int K = config_.n_states;

    smoothed_probs = Eigen::MatrixXd::Zero(T, K);
    smoothed_probs.row(T - 1) = filtered_probs.row(T - 1);

    for (int t = T - 2; t >= 0; --t) {
        // Predicted probability at t+1
        Eigen::VectorXd pred_probs = transition_matrix_.transpose() * filtered_probs.row(t).transpose();

        for (int i = 0; i < K; ++i) {
            double sum = 0.0;
            for (int j = 0; j < K; ++j) {
                double pred_j = std::max(pred_probs(j), 1e-300);
                sum += transition_matrix_(i, j) * smoothed_probs(t + 1, j) / pred_j;
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

Result<MarkovSwitchingResult> MarkovSwitching::fit(const std::vector<double>& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    {
        auto valid = validation::validate_time_series(data, 20, "MarkovSwitching");
        if (valid.is_error()) return make_error<MarkovSwitchingResult>(
            valid.error()->code(), valid.error()->what(), "MarkovSwitching");
    }

    int T = static_cast<int>(data.size());
    int K = config_.n_states;

    DEBUG("[MarkovSwitching::fit] entry: T=" << T << " K=" << K);

    initialize_parameters(data);

    double prev_ll = -std::numeric_limits<double>::infinity();
    bool converged = false;
    int iter = 0;

    for (iter = 0; iter < config_.max_iterations; ++iter) {
        // E-step: Hamilton filter + Kim smoother
        Eigen::MatrixXd filtered_probs;
        double ll = hamilton_filter(data, filtered_probs);

        Eigen::MatrixXd smoothed;
        kim_smoother(filtered_probs, smoothed);

        double delta = std::abs(ll - prev_ll);
        TRACE("[MarkovSwitching::fit] iter=" << iter << " ll=" << ll << " delta=" << delta);

        if (delta < config_.tolerance && iter > 0) {
            converged = true;
            smoothed_probs_ = smoothed;
            log_likelihood_ = ll;
            break;
        }
        prev_ll = ll;

        // M-step: Update parameters

        // Update state means and variances
        for (int k = 0; k < K; ++k) {
            double gamma_sum = 0.0;
            double weighted_sum = 0.0;
            for (int t = 0; t < T; ++t) {
                gamma_sum += smoothed(t, k);
                weighted_sum += smoothed(t, k) * data[t];
            }

            if (gamma_sum > 1e-10) {
                state_means_(k) = weighted_sum / gamma_sum;

                double var_sum = 0.0;
                for (int t = 0; t < T; ++t) {
                    double diff = data[t] - state_means_(k);
                    var_sum += smoothed(t, k) * diff * diff;
                }
                state_variances_(k) = var_sum / gamma_sum + 1e-6;
            }
        }

        // Update transition matrix
        for (int i = 0; i < K; ++i) {
            double from_i_sum = 0.0;
            for (int t = 0; t < T - 1; ++t) {
                from_i_sum += smoothed(t, i);
            }

            if (from_i_sum > 1e-10) {
                for (int j = 0; j < K; ++j) {
                    // Approximate xi from smoothed and filtered probs
                    double xi_sum = 0.0;
                    for (int t = 0; t < T - 1; ++t) {
                        double emit_j = std::exp(emission_log_prob(data[t + 1], j));
                        Eigen::VectorXd pred = transition_matrix_.transpose() * filtered_probs.row(t).transpose();
                        double pred_j = std::max(pred(j), 1e-300);
                        xi_sum += smoothed(t, i) * transition_matrix_(i, j) * emit_j *
                                  smoothed(t + 1, j) / (pred_j * std::max(emit_j, 1e-300));
                    }
                    transition_matrix_(i, j) = std::max(xi_sum / from_i_sum, 1e-6);
                }
                // Normalize row
                double row_sum = transition_matrix_.row(i).sum();
                transition_matrix_.row(i) /= row_sum;
            }
        }

        // Update initial probabilities
        state_probs_ = smoothed.row(0).transpose();

        smoothed_probs_ = smoothed;
        log_likelihood_ = ll;
    }

    if (!converged) {
        WARN("[MarkovSwitching::fit] did not converge after " << config_.max_iterations << " iterations");
    }

    initialized_ = true;

    // Build result
    MarkovSwitchingResult result;
    result.state_means = state_means_;
    result.state_variances = state_variances_;
    result.transition_matrix = transition_matrix_;
    result.smoothed_probabilities = smoothed_probs_;
    result.log_likelihood = log_likelihood_;
    result.converged = converged;
    result.n_iterations = iter;

    // Decode states
    result.decoded_states.resize(T);
    for (int t = 0; t < T; ++t) {
        int max_idx;
        smoothed_probs_.row(t).maxCoeff(&max_idx);
        result.decoded_states[t] = max_idx;
    }

    // Build convergence info
    last_convergence_info_ = ConvergenceInfo{};
    last_convergence_info_.iterations = result.n_iterations;
    last_convergence_info_.converged = result.converged;
    last_convergence_info_.termination_reason = result.converged ? "tolerance" : "max_iterations";
    if (!last_convergence_info_.objective_history.empty()) {
        last_convergence_info_.final_tolerance = 0.0;
    }
    last_convergence_info_.objective_history.push_back(result.log_likelihood);

    DEBUG("[MarkovSwitching::fit] exit: converged=" << converged);

    return Result<MarkovSwitchingResult>(std::move(result));
}

Result<std::vector<int>> MarkovSwitching::decode() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<std::vector<int>>(
            ErrorCode::NOT_INITIALIZED, "MarkovSwitching has not been fitted", "MarkovSwitching");
    }

    int T = smoothed_probs_.rows();
    std::vector<int> states(T);
    for (int t = 0; t < T; ++t) {
        int max_idx;
        smoothed_probs_.row(t).maxCoeff(&max_idx);
        states[t] = max_idx;
    }

    return Result<std::vector<int>>(std::move(states));
}

Result<void> MarkovSwitching::initialize(const Eigen::VectorXd& initial_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initial_state.size() != config_.n_states) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Initial state dimension must match number of states", "MarkovSwitching");
    }

    if (!initial_state.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
            "NaN/Inf detected in initial state", "MarkovSwitching");
    }

    state_probs_ = initial_state;

    if (state_means_.size() == 0) {
        state_means_ = Eigen::VectorXd::Zero(config_.n_states);
        state_variances_ = Eigen::VectorXd::Ones(config_.n_states);
        transition_matrix_ = Eigen::MatrixXd::Constant(config_.n_states, config_.n_states,
                                                         0.05 / (config_.n_states - 1));
        for (int k = 0; k < config_.n_states; ++k) {
            state_means_(k) = static_cast<double>(k);
            transition_matrix_(k, k) = 0.95;
        }
    }

    initialized_ = true;
    return Result<void>();
}

Result<Eigen::VectorXd> MarkovSwitching::predict() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED, "MarkovSwitching has not been initialized", "MarkovSwitching");
    }

    Eigen::VectorXd next_probs = transition_matrix_.transpose() * state_probs_;
    return Result<Eigen::VectorXd>(next_probs);
}

Result<Eigen::VectorXd> MarkovSwitching::update(const Eigen::VectorXd& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED, "MarkovSwitching has not been initialized", "MarkovSwitching");
    }

    double obs = observation(0);

    // Predict
    Eigen::VectorXd predicted = transition_matrix_.transpose() * state_probs_;

    // Update with emission likelihood
    for (int k = 0; k < config_.n_states; ++k) {
        predicted(k) *= std::exp(emission_log_prob(obs, k));
    }

    double sum = predicted.sum();
    if (sum > 0) {
        state_probs_ = predicted / sum;
    }

    return Result<Eigen::VectorXd>(state_probs_);
}

Result<Eigen::VectorXd> MarkovSwitching::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED, "MarkovSwitching has not been initialized", "MarkovSwitching");
    }

    return Result<Eigen::VectorXd>(state_probs_);
}

} // namespace statistics
} // namespace trade_ngin
