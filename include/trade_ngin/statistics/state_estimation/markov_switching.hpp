#pragma once

#include "trade_ngin/statistics/base/state_estimator.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

class MarkovSwitching : public StateEstimator {
public:
    explicit MarkovSwitching(MarkovSwitchingConfig config);

    // StateEstimator interface
    Result<void> initialize(const Eigen::VectorXd& initial_state) override;
    Result<Eigen::VectorXd> predict() override;
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) override;
    Result<Eigen::VectorXd> get_state() const override;
    bool is_initialized() const override { return initialized_; }

    // Markov Switching specific
    Result<MarkovSwitchingResult> fit(const std::vector<double>& data);
    Result<std::vector<int>> decode() const;
    const ConvergenceInfo& get_convergence_info() const { return last_convergence_info_; }

    const Eigen::MatrixXd& get_transition_matrix() const { return transition_matrix_; }
    const Eigen::VectorXd& get_state_means() const { return state_means_; }
    const Eigen::VectorXd& get_state_variances() const { return state_variances_; }

private:
    MarkovSwitchingConfig config_;
    Eigen::VectorXd state_probs_;
    Eigen::VectorXd state_means_;
    Eigen::VectorXd state_variances_;
    Eigen::MatrixXd transition_matrix_;
    Eigen::MatrixXd smoothed_probs_;
    double log_likelihood_{0.0};
    bool initialized_{false};
    ConvergenceInfo last_convergence_info_;
    mutable std::mutex mutex_;

    void initialize_parameters(const std::vector<double>& data);
    double emission_log_prob(double obs, int state) const;

    // Hamilton filter (forward) and Kim smoother (backward)
    double hamilton_filter(const std::vector<double>& data,
                          Eigen::MatrixXd& filtered_probs) const;
    void kim_smoother(const Eigen::MatrixXd& filtered_probs,
                      Eigen::MatrixXd& smoothed_probs) const;
};

} // namespace statistics
} // namespace trade_ngin
