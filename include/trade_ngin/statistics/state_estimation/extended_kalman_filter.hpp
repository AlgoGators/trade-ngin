#pragma once

#include "trade_ngin/statistics/base/state_estimator.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <functional>
#include <mutex>

namespace trade_ngin {
namespace statistics {

class ExtendedKalmanFilter : public StateEstimator {
public:
    explicit ExtendedKalmanFilter(ExtendedKalmanFilterConfig config);

    Result<void> initialize(const Eigen::VectorXd& initial_state) override;
    Result<Eigen::VectorXd> predict() override;
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) override;
    Result<Eigen::VectorXd> get_state() const override;
    bool is_initialized() const override { return initialized_; }

    // Set nonlinear transition function f(x) and its Jacobian
    using TransitionFunc = std::function<Eigen::VectorXd(const Eigen::VectorXd&)>;
    using JacobianFunc = std::function<Eigen::MatrixXd(const Eigen::VectorXd&)>;

    Result<void> set_transition_function(TransitionFunc f, JacobianFunc F_jacobian);
    Result<void> set_observation_function(TransitionFunc h, JacobianFunc H_jacobian);
    Result<void> set_process_noise(const Eigen::MatrixXd& Q);
    Result<void> set_measurement_noise(const Eigen::MatrixXd& R);

    const Eigen::MatrixXd& get_state_covariance() const { return P_; }

private:
    ExtendedKalmanFilterConfig config_;

    Eigen::VectorXd x_;
    Eigen::MatrixXd P_;
    Eigen::MatrixXd Q_;
    Eigen::MatrixXd R_;

    TransitionFunc f_;
    JacobianFunc F_jacobian_;
    TransitionFunc h_;
    JacobianFunc H_jacobian_;

    bool initialized_{false};
    bool transition_set_{false};
    bool observation_set_{false};
    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
