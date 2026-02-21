#pragma once

#include "trade_ngin/statistics/base/state_estimator.hpp"
#include "trade_ngin/statistics/statistics_common.hpp"
#include <mutex>

namespace trade_ngin {
namespace statistics {

/**
 * @brief Kalman Filter for state estimation
 */
class KalmanFilter : public StateEstimator {
public:
    explicit KalmanFilter(KalmanFilterConfig config);

    Result<void> initialize(const Eigen::VectorXd& initial_state) override;
    Result<Eigen::VectorXd> predict() override;
    Result<Eigen::VectorXd> update(const Eigen::VectorXd& observation) override;
    Result<Eigen::VectorXd> get_state() const override;
    bool is_initialized() const override { return initialized_; }

    /**
     * @brief Set transition matrix (state dynamics)
     * @param F Matrix of dimensions state_dim x state_dim
     * @return Result indicating success or validation failure
     */
    Result<void> set_transition_matrix(const Eigen::MatrixXd& F);

    /**
     * @brief Set observation matrix
     * @param H Matrix of dimensions obs_dim x state_dim
     * @return Result indicating success or validation failure
     */
    Result<void> set_observation_matrix(const Eigen::MatrixXd& H);

    /**
     * @brief Set process noise covariance
     * @param Q Positive-definite matrix of dimensions state_dim x state_dim
     * @return Result indicating success or validation failure
     */
    Result<void> set_process_noise(const Eigen::MatrixXd& Q);

    /**
     * @brief Set measurement noise covariance
     * @param R Positive-definite matrix of dimensions obs_dim x obs_dim
     * @return Result indicating success or validation failure
     */
    Result<void> set_measurement_noise(const Eigen::MatrixXd& R);

    const Eigen::MatrixXd& get_state_covariance() const { return P_; }

private:
    KalmanFilterConfig config_;

    // State variables
    Eigen::VectorXd x_;         // State estimate
    Eigen::MatrixXd P_;         // State covariance

    // Model matrices
    Eigen::MatrixXd F_;         // State transition matrix
    Eigen::MatrixXd H_;         // Observation matrix
    Eigen::MatrixXd Q_;         // Process noise covariance
    Eigen::MatrixXd R_;         // Measurement noise covariance

    bool initialized_{false};
    mutable std::mutex mutex_;
};

} // namespace statistics
} // namespace trade_ngin
