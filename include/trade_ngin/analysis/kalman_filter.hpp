#pragma once

#include "trade_ngin/core/error.hpp"
#include "trade_ngin/core/types.hpp"
#include <Eigen/Dense>
#include <vector>

namespace trade_ngin {
namespace analysis {

/**
 * @brief Kalman Filter state and covariance
 */
struct KalmanState {
    Eigen::VectorXd state;           // State estimate
    Eigen::MatrixXd covariance;      // State covariance matrix
    double log_likelihood;           // Cumulative log-likelihood
    int n_observations;              // Number of observations processed
};

/**
 * @brief Kalman Filter for dynamic state estimation
 *
 * Implements the standard Kalman Filter algorithm for linear dynamical systems:
 *   State equation: x_t = F*x_{t-1} + w_t, w_t ~ N(0, Q)
 *   Observation equation: y_t = H*x_t + v_t, v_t ~ N(0, R)
 *
 * where:
 *   x_t: state vector (n_states x 1)
 *   y_t: observation vector (n_obs x 1)
 *   F: state transition matrix (n_states x n_states)
 *   H: observation matrix (n_obs x n_states)
 *   Q: process noise covariance (n_states x n_states)
 *   R: observation noise covariance (n_obs x n_obs)
 *
 * Use cases in trading:
 * - Adaptive parameter tracking (e.g., beta, spread coefficients)
 * - State-space modeling of price dynamics
 * - Cointegration relationship tracking
 * - Signal filtering and smoothing
 */
class KalmanFilter {
public:
    /**
     * @brief Constructor
     * @param n_states Number of state variables
     * @param n_obs Number of observation variables
     */
    KalmanFilter(int n_states, int n_obs);

    /**
     * @brief Initialize filter with system matrices
     * @param F State transition matrix (n_states x n_states)
     * @param H Observation matrix (n_obs x n_states)
     * @param Q Process noise covariance (n_states x n_states)
     * @param R Observation noise covariance (n_obs x n_obs)
     * @param x0 Initial state estimate (n_states x 1)
     * @param P0 Initial state covariance (n_states x n_states)
     * @return Result indicating success or error
     */
    Result<void> initialize(
        const Eigen::MatrixXd& F,
        const Eigen::MatrixXd& H,
        const Eigen::MatrixXd& Q,
        const Eigen::MatrixXd& R,
        const Eigen::VectorXd& x0,
        const Eigen::MatrixXd& P0
    );

    /**
     * @brief Predict step (time update)
     *
     * Propagates state and covariance forward in time:
     *   x_t|t-1 = F * x_{t-1|t-1}
     *   P_t|t-1 = F * P_{t-1|t-1} * F' + Q
     *
     * @return Predicted state
     */
    Result<KalmanState> predict();

    /**
     * @brief Update step (measurement update)
     *
     * Incorporates new observation to update state estimate:
     *   K_t = P_t|t-1 * H' * (H * P_t|t-1 * H' + R)^(-1)
     *   x_t|t = x_t|t-1 + K_t * (y_t - H * x_t|t-1)
     *   P_t|t = (I - K_t * H) * P_t|t-1
     *
     * @param observation New observation vector
     * @return Updated state
     */
    Result<KalmanState> update(const Eigen::VectorXd& observation);

    /**
     * @brief Filter step (predict + update combined)
     * @param observation New observation vector
     * @return Updated state after filtering
     */
    Result<KalmanState> filter(const Eigen::VectorXd& observation);

    /**
     * @brief Batch filtering of multiple observations
     * @param observations Matrix where each row is an observation
     * @return Vector of filtered states
     */
    Result<std::vector<KalmanState>> filter_batch(const Eigen::MatrixXd& observations);

    /**
     * @brief Kalman smoothing (Rauch-Tung-Striebel algorithm)
     *
     * Runs backward pass to compute smoothed state estimates using all data.
     * Must call filter_batch() first to generate forward pass.
     *
     * @return Vector of smoothed states
     */
    Result<std::vector<KalmanState>> smooth();

    /**
     * @brief Get current state estimate
     * @return Current state vector
     */
    const Eigen::VectorXd& get_state() const { return x_; }

    /**
     * @brief Get current state covariance
     * @return Current covariance matrix
     */
    const Eigen::MatrixXd& get_covariance() const { return P_; }

    /**
     * @brief Get Kalman gain from last update
     * @return Kalman gain matrix
     */
    const Eigen::MatrixXd& get_kalman_gain() const { return K_; }

    /**
     * @brief Get innovation (prediction error) from last update
     * @return Innovation vector
     */
    const Eigen::VectorXd& get_innovation() const { return innovation_; }

    /**
     * @brief Get cumulative log-likelihood
     * @return Log-likelihood value
     */
    double get_log_likelihood() const { return log_likelihood_; }

    /**
     * @brief Reset filter to initial state
     */
    void reset();

    /**
     * @brief Check if filter has been initialized
     * @return True if initialize() has been called
     */
    bool is_initialized() const { return initialized_; }

private:
    int n_states_;
    int n_obs_;
    bool initialized_;

    // System matrices
    Eigen::MatrixXd F_;  // State transition
    Eigen::MatrixXd H_;  // Observation
    Eigen::MatrixXd Q_;  // Process noise covariance
    Eigen::MatrixXd R_;  // Observation noise covariance

    // State variables
    Eigen::VectorXd x_;  // State estimate
    Eigen::MatrixXd P_;  // State covariance
    Eigen::MatrixXd K_;  // Kalman gain
    Eigen::VectorXd innovation_;  // Innovation vector

    // Initial state
    Eigen::VectorXd x0_;
    Eigen::MatrixXd P0_;

    // Statistics
    double log_likelihood_;
    int n_observations_;

    // Storage for smoothing
    std::vector<Eigen::VectorXd> filtered_states_;
    std::vector<Eigen::MatrixXd> filtered_covariances_;
    std::vector<Eigen::VectorXd> predicted_states_;
    std::vector<Eigen::MatrixXd> predicted_covariances_;
};

/**
 * @brief Simple helper: Create Kalman Filter for pairs trading (spread tracking)
 *
 * Models the spread between two cointegrated assets as a mean-reverting process.
 * State: [spread_level, spread_velocity]
 * Observation: current spread value
 *
 * @param process_noise Process noise standard deviation
 * @param observation_noise Observation noise standard deviation
 * @return Configured Kalman Filter
 */
Result<KalmanFilter> create_pairs_trading_filter(
    double process_noise = 0.01,
    double observation_noise = 0.1
);

/**
 * @brief Simple helper: Create Kalman Filter for adaptive beta estimation
 *
 * Tracks time-varying beta coefficient in linear regression.
 * State: [beta]
 * Observation: y (with x as known input)
 *
 * @param process_noise Process noise standard deviation
 * @param observation_noise Observation noise standard deviation
 * @return Configured Kalman Filter
 */
Result<KalmanFilter> create_adaptive_beta_filter(
    double process_noise = 0.001,
    double observation_noise = 0.1
);

} // namespace analysis
} // namespace trade_ngin
