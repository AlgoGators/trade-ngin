#pragma once

#include "trade_ngin/statistics/base/state_estimator.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Hidden Markov Model for regime detection
 */
class HMM : public StateEstimator {
public:
    explicit HMM(HMMConfig config);

    Result<void> initialize(const Eigen::VectorXd& initial_state) override;
    Result<Eigen::VectorXd> predict() override;
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) override;
    Result<Eigen::VectorXd> get_state() const override;
    bool is_initialized() const override { return initialized_; }

    /**
     * @brief Fit HMM to observation sequence using Baum-Welch algorithm
     * @param observations Matrix of observations (time steps x features)
     * @return Result indicating success or failure
     */
    Result<void> fit(const Eigen::MatrixXd& observations);

    Result<ConvergenceInfo> fit_with_diagnostics(const Eigen::MatrixXd& observations);
    const ConvergenceInfo& get_convergence_info() const { return last_convergence_info_; }

    /**
     * @brief Decode most likely state sequence (Viterbi algorithm)
     * @param observations Matrix of observations
     * @return Vector of most likely states
     */
    Result<std::vector<int>> decode(const Eigen::MatrixXd& observations) const;

    /**
     * @brief Get current state probabilities
     */
    const Eigen::VectorXd& get_state_probabilities() const { return state_probs_; }

    /**
     * @brief Get transition matrix
     */
    const Eigen::MatrixXd& get_transition_matrix() const { return transition_matrix_; }

    /**
     * @brief Get emission parameters (Gaussian means and covariances)
     */
    const std::vector<Eigen::VectorXd>& get_means() const { return means_; }
    const std::vector<Eigen::MatrixXd>& get_covariances() const { return covariances_; }

private:
    HMMConfig config_;

    // HMM parameters
    Eigen::VectorXd state_probs_;           // Current state probabilities
    Eigen::VectorXd initial_probs_;         // Initial state distribution
    Eigen::MatrixXd transition_matrix_;     // State transition probabilities
    std::vector<Eigen::VectorXd> means_;    // Emission means for each state
    std::vector<Eigen::MatrixXd> covariances_; // Emission covariances

    bool initialized_{false};
    ConvergenceInfo last_convergence_info_;
    mutable std::mutex mutex_;

    // Helper methods for Baum-Welch
    void initialize_parameters(const Eigen::MatrixXd& observations);
    double forward_backward(const Eigen::MatrixXd& observations,
                           Eigen::MatrixXd& gamma,
                           Eigen::MatrixXd& xi) const;
    double log_emission_probability(const Eigen::VectorXd& obs, int state) const;
    double emission_probability(const Eigen::VectorXd& obs, int state) const;
};

} // namespace statistics
} // namespace trade_ngin
