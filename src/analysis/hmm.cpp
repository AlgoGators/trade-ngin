#include "trade_ngin/analysis/hmm.hpp"
#include "trade_ngin/analysis/preprocessing.hpp"
#include <Eigen/Eigenvalues>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace trade_ngin {
namespace analysis {

namespace {
    const double LOG_EPSILON = -1e10; // Small value for log(0)
    const double EPSILON = 1e-10;

    // Numerical stable log-sum-exp
    double log_sum_exp(const Eigen::VectorXd& log_values) {
        double max_val = log_values.maxCoeff();
        if (max_val == LOG_EPSILON) {
            return LOG_EPSILON;
        }
        double sum = 0.0;
        for (int i = 0; i < log_values.size(); ++i) {
            if (log_values(i) != LOG_EPSILON) {
                sum += std::exp(log_values(i) - max_val);
            }
        }
        return max_val + std::log(sum);
    }
}

HMM::HMM(int n_states, int n_features)
    : n_states_(n_states)
    , n_features_(n_features)
    , fitted_(false) {

    means_.resize(n_states);
    covariances_.resize(n_states);
}

void HMM::initialize_parameters(const Eigen::MatrixXd& observations) {
    int n_obs = observations.rows();

    // Initialize transition matrix (uniform with self-loop bias)
    transition_matrix_ = Eigen::MatrixXd::Constant(n_states_, n_states_, 0.1 / (n_states_ - 1));
    for (int i = 0; i < n_states_; ++i) {
        transition_matrix_(i, i) = 0.9;
    }

    // Initialize initial distribution (uniform)
    initial_distribution_ = Eigen::VectorXd::Constant(n_states_, 1.0 / n_states_);

    // Initialize emission parameters using k-means-like clustering
    // Partition observations into n_states groups
    int partition_size = n_obs / n_states_;

    for (int k = 0; k < n_states_; ++k) {
        int start_idx = k * partition_size;
        int end_idx = (k == n_states_ - 1) ? n_obs : (k + 1) * partition_size;

        // Calculate mean for this partition
        means_[k] = Eigen::VectorXd::Zero(n_features_);
        for (int i = start_idx; i < end_idx; ++i) {
            means_[k] += observations.row(i).transpose();
        }
        means_[k] /= (end_idx - start_idx);

        // Calculate covariance for this partition
        covariances_[k] = Eigen::MatrixXd::Zero(n_features_, n_features_);
        for (int i = start_idx; i < end_idx; ++i) {
            Eigen::VectorXd diff = observations.row(i).transpose() - means_[k];
            covariances_[k] += diff * diff.transpose();
        }
        covariances_[k] /= (end_idx - start_idx);

        // Add regularization to ensure positive definite
        covariances_[k] += EPSILON * Eigen::MatrixXd::Identity(n_features_, n_features_);
    }

    current_state_probs_ = initial_distribution_;
}

double HMM::gaussian_log_pdf(
    const Eigen::VectorXd& x,
    const Eigen::VectorXd& mean,
    const Eigen::MatrixXd& covariance) const {

    int n = x.size();
    Eigen::VectorXd diff = x - mean;

    // Use LLT decomposition for numerical stability
    Eigen::LLT<Eigen::MatrixXd> llt(covariance);
    if (llt.info() != Eigen::Success) {
        return LOG_EPSILON;
    }

    double log_det = 2.0 * llt.matrixL().diagonal().array().log().sum();
    Eigen::VectorXd solved = llt.solve(diff);
    double mahalanobis = diff.dot(solved);

    return -0.5 * (n * std::log(2.0 * M_PI) + log_det + mahalanobis);
}

Result<Eigen::MatrixXd> HMM::forward_algorithm(const Eigen::MatrixXd& observations) {
    int n_obs = observations.rows();
    Eigen::MatrixXd alpha(n_obs, n_states_);

    // Initialize
    for (int k = 0; k < n_states_; ++k) {
        double log_emission = gaussian_log_pdf(observations.row(0), means_[k], covariances_[k]);
        alpha(0, k) = std::log(initial_distribution_(k) + EPSILON) + log_emission;
    }

    // Forward recursion
    for (int t = 1; t < n_obs; ++t) {
        for (int j = 0; j < n_states_; ++j) {
            Eigen::VectorXd log_transition(n_states_);
            for (int i = 0; i < n_states_; ++i) {
                log_transition(i) = alpha(t-1, i) + std::log(transition_matrix_(i, j) + EPSILON);
            }

            double log_emission = gaussian_log_pdf(observations.row(t), means_[j], covariances_[j]);
            alpha(t, j) = log_sum_exp(log_transition) + log_emission;
        }
    }

    return Result<Eigen::MatrixXd>(alpha);
}

Result<Eigen::MatrixXd> HMM::backward_algorithm(const Eigen::MatrixXd& observations) {
    int n_obs = observations.rows();
    Eigen::MatrixXd beta(n_obs, n_states_);

    // Initialize
    beta.row(n_obs - 1).setZero();

    // Backward recursion
    for (int t = n_obs - 2; t >= 0; --t) {
        for (int i = 0; i < n_states_; ++i) {
            Eigen::VectorXd log_values(n_states_);
            for (int j = 0; j < n_states_; ++j) {
                double log_emission = gaussian_log_pdf(observations.row(t+1), means_[j], covariances_[j]);
                log_values(j) = std::log(transition_matrix_(i, j) + EPSILON) +
                               log_emission + beta(t+1, j);
            }
            beta(t, i) = log_sum_exp(log_values);
        }
    }

    return Result<Eigen::MatrixXd>(beta);
}

Result<std::vector<int>> HMM::viterbi_algorithm(const Eigen::MatrixXd& observations) {
    int n_obs = observations.rows();
    Eigen::MatrixXd delta(n_obs, n_states_);
    Eigen::MatrixXi psi(n_obs, n_states_);

    // Initialize
    for (int k = 0; k < n_states_; ++k) {
        double log_emission = gaussian_log_pdf(observations.row(0), means_[k], covariances_[k]);
        delta(0, k) = std::log(initial_distribution_(k) + EPSILON) + log_emission;
        psi(0, k) = 0;
    }

    // Forward pass
    for (int t = 1; t < n_obs; ++t) {
        for (int j = 0; j < n_states_; ++j) {
            double max_val = LOG_EPSILON;
            int max_state = 0;

            for (int i = 0; i < n_states_; ++i) {
                double val = delta(t-1, i) + std::log(transition_matrix_(i, j) + EPSILON);
                if (val > max_val) {
                    max_val = val;
                    max_state = i;
                }
            }

            double log_emission = gaussian_log_pdf(observations.row(t), means_[j], covariances_[j]);
            delta(t, j) = max_val + log_emission;
            psi(t, j) = max_state;
        }
    }

    // Backtrack
    std::vector<int> states(n_obs);
    int max_idx;
    delta.row(n_obs - 1).maxCoeff(&max_idx);
    states[n_obs - 1] = max_idx;

    for (int t = n_obs - 2; t >= 0; --t) {
        states[t] = psi(t + 1, states[t + 1]);
    }

    return Result<std::vector<int>>(states);
}

Result<void> HMM::em_step(
    const Eigen::MatrixXd& observations,
    Eigen::MatrixXd& alpha,
    Eigen::MatrixXd& beta,
    Eigen::MatrixXd& gamma,
    std::vector<Eigen::MatrixXd>& xi) {

    int n_obs = observations.rows();

    // E-step: Calculate gamma and xi
    gamma = Eigen::MatrixXd::Zero(n_obs, n_states_);
    xi.resize(n_obs - 1);

    for (int t = 0; t < n_obs; ++t) {
        Eigen::VectorXd log_gamma(n_states_);
        for (int k = 0; k < n_states_; ++k) {
            log_gamma(k) = alpha(t, k) + beta(t, k);
        }
        double log_norm = log_sum_exp(log_gamma);

        for (int k = 0; k < n_states_; ++k) {
            gamma(t, k) = std::exp(log_gamma(k) - log_norm);
        }
    }

    for (int t = 0; t < n_obs - 1; ++t) {
        xi[t] = Eigen::MatrixXd::Zero(n_states_, n_states_);

        for (int i = 0; i < n_states_; ++i) {
            for (int j = 0; j < n_states_; ++j) {
                double log_emission = gaussian_log_pdf(observations.row(t+1), means_[j], covariances_[j]);
                xi[t](i, j) = alpha(t, i) +
                             std::log(transition_matrix_(i, j) + EPSILON) +
                             log_emission +
                             beta(t+1, j);
            }
        }

        double log_norm = LOG_EPSILON;
        for (int i = 0; i < n_states_; ++i) {
            for (int j = 0; j < n_states_; ++j) {
                if (log_norm == LOG_EPSILON) {
                    log_norm = xi[t](i, j);
                } else {
                    double max_val = std::max(log_norm, xi[t](i, j));
                    log_norm = max_val + std::log(std::exp(log_norm - max_val) + std::exp(xi[t](i, j) - max_val));
                }
            }
        }

        for (int i = 0; i < n_states_; ++i) {
            for (int j = 0; j < n_states_; ++j) {
                xi[t](i, j) = std::exp(xi[t](i, j) - log_norm);
            }
        }
    }

    // M-step: Update parameters
    // Update initial distribution
    initial_distribution_ = gamma.row(0).transpose();

    // Update transition matrix
    for (int i = 0; i < n_states_; ++i) {
        double denom = 0.0;
        for (int t = 0; t < n_obs - 1; ++t) {
            denom += gamma(t, i);
        }

        for (int j = 0; j < n_states_; ++j) {
            double numer = 0.0;
            for (int t = 0; t < n_obs - 1; ++t) {
                numer += xi[t](i, j);
            }
            transition_matrix_(i, j) = (numer + EPSILON) / (denom + EPSILON * n_states_);
        }
    }

    // Update emission parameters
    for (int k = 0; k < n_states_; ++k) {
        double gamma_sum = gamma.col(k).sum();

        // Update mean
        means_[k] = Eigen::VectorXd::Zero(n_features_);
        for (int t = 0; t < n_obs; ++t) {
            means_[k] += gamma(t, k) * observations.row(t).transpose();
        }
        means_[k] /= (gamma_sum + EPSILON);

        // Update covariance
        covariances_[k] = Eigen::MatrixXd::Zero(n_features_, n_features_);
        for (int t = 0; t < n_obs; ++t) {
            Eigen::VectorXd diff = observations.row(t).transpose() - means_[k];
            covariances_[k] += gamma(t, k) * diff * diff.transpose();
        }
        covariances_[k] /= (gamma_sum + EPSILON);
        covariances_[k] += EPSILON * Eigen::MatrixXd::Identity(n_features_, n_features_);
    }

    return Result<void>();
}

Result<HMMFitResult> HMM::fit(
    const Eigen::MatrixXd& observations,
    int max_iterations,
    double tolerance) {

    if (observations.rows() < n_states_ * 2) {
        return make_error<HMMFitResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Need more observations than states for HMM fitting",
            "HMM::fit"
        );
    }

    if (observations.cols() != n_features_) {
        return make_error<HMMFitResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Observation dimensionality does not match n_features",
            "HMM::fit"
        );
    }

    // Initialize parameters
    initialize_parameters(observations);

    double prev_log_likelihood = LOG_EPSILON;
    bool converged = false;
    int iter = 0;

    // EM algorithm
    for (iter = 0; iter < max_iterations; ++iter) {
        // Forward-backward
        auto alpha_result = forward_algorithm(observations);
        if (!alpha_result) {
            return make_error<HMMFitResult>(
                alpha_result.error().code,
                alpha_result.error().message,
                "HMM::fit"
            );
        }
        Eigen::MatrixXd alpha = alpha_result.value();

        auto beta_result = backward_algorithm(observations);
        if (!beta_result) {
            return make_error<HMMFitResult>(
                beta_result.error().code,
                beta_result.error().message,
                "HMM::fit"
            );
        }
        Eigen::MatrixXd beta = beta_result.value();

        // Calculate log-likelihood
        Eigen::VectorXd log_probs = alpha.row(observations.rows() - 1);
        double log_likelihood = log_sum_exp(log_probs);

        // Check convergence
        if (std::abs(log_likelihood - prev_log_likelihood) < tolerance) {
            converged = true;
            break;
        }

        prev_log_likelihood = log_likelihood;

        // EM step
        Eigen::MatrixXd gamma;
        std::vector<Eigen::MatrixXd> xi;
        auto em_result = em_step(observations, alpha, beta, gamma, xi);
        if (!em_result) {
            return make_error<HMMFitResult>(
                em_result.error().code,
                em_result.error().message,
                "HMM::fit"
            );
        }
    }

    fitted_ = true;

    // Get Viterbi path
    auto viterbi_result = viterbi_algorithm(observations);
    if (!viterbi_result) {
        return make_error<HMMFitResult>(
            viterbi_result.error().code,
            viterbi_result.error().message,
            "HMM::fit"
        );
    }

    // Get state probabilities
    auto proba_result = predict_proba(observations);
    if (!proba_result) {
        return make_error<HMMFitResult>(
            proba_result.error().code,
            proba_result.error().message,
            "HMM::fit"
        );
    }

    HMMFitResult result;
    result.transition_matrix = transition_matrix_;
    result.initial_distribution = initial_distribution_;
    result.emission_means = means_;
    result.emission_covariances = covariances_;
    result.log_likelihood = prev_log_likelihood;
    result.state_sequence = viterbi_result.value();
    result.state_probabilities = proba_result.value();
    result.converged = converged;
    result.n_iterations = iter;

    return Result<HMMFitResult>(result);
}

Result<HMMFitResult> HMM::fit(
    const std::vector<double>& returns,
    int max_iterations,
    double tolerance) {

    // Convert to matrix (1D observations)
    Eigen::MatrixXd observations(returns.size(), 1);
    for (size_t i = 0; i < returns.size(); ++i) {
        observations(i, 0) = returns[i];
    }

    return fit(observations, max_iterations, tolerance);
}

Result<std::vector<int>> HMM::predict(const Eigen::MatrixXd& observations) {
    if (!fitted_) {
        return make_error<std::vector<int>>(
            ErrorCode::INVALID_STATE,
            "HMM must be fitted before prediction",
            "HMM::predict"
        );
    }

    return viterbi_algorithm(observations);
}

Result<Eigen::MatrixXd> HMM::predict_proba(const Eigen::MatrixXd& observations) {
    if (!fitted_) {
        return make_error<Eigen::MatrixXd>(
            ErrorCode::INVALID_STATE,
            "HMM must be fitted before prediction",
            "HMM::predict_proba"
        );
    }

    auto alpha_result = forward_algorithm(observations);
    if (!alpha_result) {
        return make_error<Eigen::MatrixXd>(
            alpha_result.error().code,
            alpha_result.error().message,
            "HMM::predict_proba"
        );
    }
    Eigen::MatrixXd alpha = alpha_result.value();

    auto beta_result = backward_algorithm(observations);
    if (!beta_result) {
        return make_error<Eigen::MatrixXd>(
            beta_result.error().code,
            beta_result.error().message,
            "HMM::predict_proba"
        );
    }
    Eigen::MatrixXd beta = beta_result.value();

    int n_obs = observations.rows();
    Eigen::MatrixXd gamma(n_obs, n_states_);

    for (int t = 0; t < n_obs; ++t) {
        Eigen::VectorXd log_gamma(n_states_);
        for (int k = 0; k < n_states_; ++k) {
            log_gamma(k) = alpha(t, k) + beta(t, k);
        }
        double log_norm = log_sum_exp(log_gamma);

        for (int k = 0; k < n_states_; ++k) {
            gamma(t, k) = std::exp(log_gamma(k) - log_norm);
        }
    }

    return Result<Eigen::MatrixXd>(gamma);
}

Result<int> HMM::predict_state(const Eigen::VectorXd& observation) {
    if (!fitted_) {
        return make_error<int>(
            ErrorCode::INVALID_STATE,
            "HMM must be fitted before prediction",
            "HMM::predict_state"
        );
    }

    Eigen::MatrixXd obs(1, observation.size());
    obs.row(0) = observation.transpose();

    auto states_result = viterbi_algorithm(obs);
    if (!states_result) {
        return make_error<int>(
            states_result.error().code,
            states_result.error().message,
            "HMM::predict_state"
        );
    }

    return Result<int>(states_result.value()[0]);
}

Result<double> HMM::score(const Eigen::MatrixXd& observations) {
    if (!fitted_) {
        return make_error<double>(
            ErrorCode::INVALID_STATE,
            "HMM must be fitted before scoring",
            "HMM::score"
        );
    }

    auto alpha_result = forward_algorithm(observations);
    if (!alpha_result) {
        return make_error<double>(
            alpha_result.error().code,
            alpha_result.error().message,
            "HMM::score"
        );
    }

    Eigen::VectorXd log_probs = alpha_result.value().row(observations.rows() - 1);
    return Result<double>(log_sum_exp(log_probs));
}

std::pair<Eigen::VectorXd, Eigen::MatrixXd> HMM::get_emission_params(int state) const {
    if (state < 0 || state >= n_states_) {
        return {Eigen::VectorXd(), Eigen::MatrixXd()};
    }
    return {means_[state], covariances_[state]};
}

Result<Eigen::VectorXd> HMM::get_stationary_distribution() const {
    if (!fitted_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::INVALID_STATE,
            "HMM must be fitted to get stationary distribution",
            "HMM::get_stationary_distribution"
        );
    }

    // Find eigenvector corresponding to eigenvalue 1
    Eigen::EigenSolver<Eigen::MatrixXd> eigen_solver(transition_matrix_.transpose());

    int stationary_idx = -1;
    for (int i = 0; i < n_states_; ++i) {
        if (std::abs(eigen_solver.eigenvalues()(i).real() - 1.0) < 1e-6) {
            stationary_idx = i;
            break;
        }
    }

    if (stationary_idx == -1) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::CALCULATION_ERROR,
            "Could not find stationary distribution",
            "HMM::get_stationary_distribution"
        );
    }

    Eigen::VectorXd stationary = eigen_solver.eigenvectors().col(stationary_idx).real();
    stationary = stationary.array().abs();
    stationary /= stationary.sum();

    return Result<Eigen::VectorXd>(stationary);
}

// Helper functions
Result<HMMFitResult> detect_market_regimes(const std::vector<double>& prices, int window) {
    if (prices.size() < static_cast<size_t>(window + 20)) {
        return make_error<HMMFitResult>(
            ErrorCode::INVALID_ARGUMENT,
            "Not enough price data for regime detection",
            "detect_market_regimes"
        );
    }

    // Calculate features: returns and realized volatility
    std::vector<double> returns;
    std::vector<double> volatility;

    for (size_t i = window; i < prices.size(); ++i) {
        // Return
        double ret = std::log(prices[i] / prices[i-1]);
        returns.push_back(ret);

        // Realized volatility (stdev of returns in window)
        double vol_sum = 0.0;
        for (int j = 0; j < window; ++j) {
            double r = std::log(prices[i-j] / prices[i-j-1]);
            vol_sum += r * r;
        }
        volatility.push_back(std::sqrt(vol_sum / window));
    }

    // Build observation matrix
    Eigen::MatrixXd observations(returns.size(), 2);
    for (size_t i = 0; i < returns.size(); ++i) {
        observations(i, 0) = returns[i];
        observations(i, 1) = volatility[i];
    }

    // Fit 3-state HMM
    HMM hmm(3, 2);
    return hmm.fit(observations);
}

Result<MarketRegime> classify_regime(
    const HMM& hmm,
    int state,
    const Eigen::MatrixXd& observations) {

    if (!hmm.is_fitted()) {
        return make_error<MarketRegime>(
            ErrorCode::INVALID_STATE,
            "HMM must be fitted for regime classification",
            "classify_regime"
        );
    }

    auto [mean, cov] = hmm.get_emission_params(state);

    if (mean.size() < 2) {
        return make_error<MarketRegime>(
            ErrorCode::INVALID_ARGUMENT,
            "HMM must have at least 2D observations for regime classification",
            "classify_regime"
        );
    }

    double mean_return = mean(0);
    double mean_volatility = mean(1);

    // Simple classification based on return and volatility characteristics
    if (mean_volatility > 0.02) {
        return Result<MarketRegime>(MarketRegime::HIGH_VOLATILITY);
    } else if (mean_volatility < 0.005) {
        return Result<MarketRegime>(MarketRegime::LOW_VOLATILITY);
    } else if (mean_return > 0.001) {
        return Result<MarketRegime>(MarketRegime::TRENDING_UP);
    } else if (mean_return < -0.001) {
        return Result<MarketRegime>(MarketRegime::TRENDING_DOWN);
    } else {
        return Result<MarketRegime>(MarketRegime::MEAN_REVERTING);
    }
}

} // namespace analysis
} // namespace trade_ngin
