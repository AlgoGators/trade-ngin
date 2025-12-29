#include "trade_ngin/analysis/kalman_filter.hpp"
#include <cmath>

namespace trade_ngin {
namespace analysis {

KalmanFilter::KalmanFilter(int n_states, int n_obs)
    : n_states_(n_states)
    , n_obs_(n_obs)
    , initialized_(false)
    , log_likelihood_(0.0)
    , n_observations_(0) {
}

Result<void> KalmanFilter::initialize(
    const Eigen::MatrixXd& F,
    const Eigen::MatrixXd& H,
    const Eigen::MatrixXd& Q,
    const Eigen::MatrixXd& R,
    const Eigen::VectorXd& x0,
    const Eigen::MatrixXd& P0)
{
    // Validate dimensions
    if (F.rows() != n_states_ || F.cols() != n_states_) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "F matrix dimensions must be n_states x n_states",
            "KalmanFilter::initialize"
        );
    }

    if (H.rows() != n_obs_ || H.cols() != n_states_) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "H matrix dimensions must be n_obs x n_states",
            "KalmanFilter::initialize"
        );
    }

    if (Q.rows() != n_states_ || Q.cols() != n_states_) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Q matrix dimensions must be n_states x n_states",
            "KalmanFilter::initialize"
        );
    }

    if (R.rows() != n_obs_ || R.cols() != n_obs_) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "R matrix dimensions must be n_obs x n_obs",
            "KalmanFilter::initialize"
        );
    }

    if (x0.size() != n_states_) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "x0 vector size must be n_states",
            "KalmanFilter::initialize"
        );
    }

    if (P0.rows() != n_states_ || P0.cols() != n_states_) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "P0 matrix dimensions must be n_states x n_states",
            "KalmanFilter::initialize"
        );
    }

    // Store system matrices
    F_ = F;
    H_ = H;
    Q_ = Q;
    R_ = R;
    x0_ = x0;
    P0_ = P0;

    // Initialize state
    x_ = x0;
    P_ = P0;

    // Initialize other variables
    K_ = Eigen::MatrixXd::Zero(n_states_, n_obs_);
    innovation_ = Eigen::VectorXd::Zero(n_obs_);
    log_likelihood_ = 0.0;
    n_observations_ = 0;

    initialized_ = true;
    return Result<void>();
}

Result<KalmanState> KalmanFilter::predict() {
    if (!initialized_) {
        return make_error<KalmanState>(
            ErrorCode::INVALID_STATE,
            "Kalman filter must be initialized before prediction",
            "KalmanFilter::predict"
        );
    }

    // Predict state: x_t|t-1 = F * x_{t-1|t-1}
    x_ = F_ * x_;

    // Predict covariance: P_t|t-1 = F * P_{t-1|t-1} * F' + Q
    P_ = F_ * P_ * F_.transpose() + Q_;

    KalmanState state;
    state.state = x_;
    state.covariance = P_;
    state.log_likelihood = log_likelihood_;
    state.n_observations = n_observations_;

    return Result<KalmanState>(state);
}

Result<KalmanState> KalmanFilter::update(const Eigen::VectorXd& observation) {
    if (!initialized_) {
        return make_error<KalmanState>(
            ErrorCode::INVALID_STATE,
            "Kalman filter must be initialized before update",
            "KalmanFilter::update"
        );
    }

    if (observation.size() != n_obs_) {
        return make_error<KalmanState>(
            ErrorCode::INVALID_ARGUMENT,
            "Observation size must match n_obs",
            "KalmanFilter::update"
        );
    }

    // Innovation: y_t - H * x_t|t-1
    innovation_ = observation - H_ * x_;

    // Innovation covariance: S = H * P_t|t-1 * H' + R
    Eigen::MatrixXd S = H_ * P_ * H_.transpose() + R_;

    // Kalman gain: K = P_t|t-1 * H' * S^(-1)
    K_ = P_ * H_.transpose() * S.inverse();

    // Update state: x_t|t = x_t|t-1 + K * innovation
    x_ = x_ + K_ * innovation_;

    // Update covariance: P_t|t = (I - K * H) * P_t|t-1
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(n_states_, n_states_);
    P_ = (I - K_ * H_) * P_;

    // Ensure P remains symmetric and positive definite
    P_ = 0.5 * (P_ + P_.transpose());

    // Update log-likelihood
    double det_S = S.determinant();
    if (det_S > 0) {
        double log_det_S = std::log(det_S);
        double mahalanobis = (innovation_.transpose() * S.inverse() * innovation_)(0);
        log_likelihood_ += -0.5 * (n_obs_ * std::log(2.0 * M_PI) + log_det_S + mahalanobis);
    }

    n_observations_++;

    KalmanState state;
    state.state = x_;
    state.covariance = P_;
    state.log_likelihood = log_likelihood_;
    state.n_observations = n_observations_;

    return Result<KalmanState>(state);
}

Result<KalmanState> KalmanFilter::filter(const Eigen::VectorXd& observation) {
    auto predict_result = predict();
    if (!predict_result) {
        return make_error<KalmanState>(
            predict_result.error().code,
            predict_result.error().message,
            "KalmanFilter::filter"
        );
    }

    return update(observation);
}

Result<std::vector<KalmanState>> KalmanFilter::filter_batch(const Eigen::MatrixXd& observations) {
    if (!initialized_) {
        return make_error<std::vector<KalmanState>>(
            ErrorCode::INVALID_STATE,
            "Kalman filter must be initialized before batch filtering",
            "KalmanFilter::filter_batch"
        );
    }

    int n_time_steps = observations.rows();
    std::vector<KalmanState> states;
    states.reserve(n_time_steps);

    // Clear storage for smoothing
    filtered_states_.clear();
    filtered_covariances_.clear();
    predicted_states_.clear();
    predicted_covariances_.clear();

    filtered_states_.reserve(n_time_steps);
    filtered_covariances_.reserve(n_time_steps);
    predicted_states_.reserve(n_time_steps);
    predicted_covariances_.reserve(n_time_steps);

    // Reset to initial state
    x_ = x0_;
    P_ = P0_;
    log_likelihood_ = 0.0;
    n_observations_ = 0;

    for (int t = 0; t < n_time_steps; ++t) {
        Eigen::VectorXd observation = observations.row(t);

        // Predict
        auto predict_result = predict();
        if (!predict_result) {
            return make_error<std::vector<KalmanState>>(
                predict_result.error().code,
                predict_result.error().message,
                "KalmanFilter::filter_batch"
            );
        }

        // Store predicted state for smoothing
        predicted_states_.push_back(x_);
        predicted_covariances_.push_back(P_);

        // Update
        auto update_result = update(observation);
        if (!update_result) {
            return make_error<std::vector<KalmanState>>(
                update_result.error().code,
                update_result.error().message,
                "KalmanFilter::filter_batch"
            );
        }

        // Store filtered state for smoothing
        filtered_states_.push_back(x_);
        filtered_covariances_.push_back(P_);

        states.push_back(update_result.value());
    }

    return Result<std::vector<KalmanState>>(states);
}

Result<std::vector<KalmanState>> KalmanFilter::smooth() {
    if (filtered_states_.empty()) {
        return make_error<std::vector<KalmanState>>(
            ErrorCode::INVALID_STATE,
            "Must call filter_batch before smoothing",
            "KalmanFilter::smooth"
        );
    }

    int n_time_steps = filtered_states_.size();
    std::vector<KalmanState> smoothed_states(n_time_steps);

    // Initialize with last filtered state
    smoothed_states[n_time_steps - 1].state = filtered_states_[n_time_steps - 1];
    smoothed_states[n_time_steps - 1].covariance = filtered_covariances_[n_time_steps - 1];
    smoothed_states[n_time_steps - 1].log_likelihood = log_likelihood_;
    smoothed_states[n_time_steps - 1].n_observations = n_time_steps;

    // Backward pass
    for (int t = n_time_steps - 2; t >= 0; --t) {
        // Smoother gain: J_t = P_t|t * F' * P_{t+1|t}^(-1)
        Eigen::MatrixXd J = filtered_covariances_[t] * F_.transpose() *
                           predicted_covariances_[t + 1].inverse();

        // Smoothed state: x_t|T = x_t|t + J_t * (x_{t+1|T} - x_{t+1|t})
        smoothed_states[t].state = filtered_states_[t] +
            J * (smoothed_states[t + 1].state - predicted_states_[t + 1]);

        // Smoothed covariance: P_t|T = P_t|t + J_t * (P_{t+1|T} - P_{t+1|t}) * J_t'
        smoothed_states[t].covariance = filtered_covariances_[t] +
            J * (smoothed_states[t + 1].covariance - predicted_covariances_[t + 1]) * J.transpose();

        smoothed_states[t].log_likelihood = log_likelihood_;
        smoothed_states[t].n_observations = t + 1;
    }

    return Result<std::vector<KalmanState>>(smoothed_states);
}

void KalmanFilter::reset() {
    if (initialized_) {
        x_ = x0_;
        P_ = P0_;
        log_likelihood_ = 0.0;
        n_observations_ = 0;
        filtered_states_.clear();
        filtered_covariances_.clear();
        predicted_states_.clear();
        predicted_covariances_.clear();
    }
}

// Helper: Create pairs trading filter
Result<KalmanFilter> create_pairs_trading_filter(
    double process_noise,
    double observation_noise)
{
    // State: [spread_level, spread_velocity]
    // Observation: current spread

    KalmanFilter filter(2, 1);

    // State transition matrix (constant velocity model)
    Eigen::MatrixXd F(2, 2);
    F << 1.0, 1.0,
         0.0, 1.0;

    // Observation matrix (observe only position)
    Eigen::MatrixXd H(1, 2);
    H << 1.0, 0.0;

    // Process noise covariance
    Eigen::MatrixXd Q(2, 2);
    Q << process_noise * process_noise, 0.0,
         0.0, process_noise * process_noise;

    // Observation noise covariance
    Eigen::MatrixXd R(1, 1);
    R << observation_noise * observation_noise;

    // Initial state
    Eigen::VectorXd x0(2);
    x0 << 0.0, 0.0;

    // Initial covariance
    Eigen::MatrixXd P0(2, 2);
    P0 << 1.0, 0.0,
          0.0, 1.0;

    auto result = filter.initialize(F, H, Q, R, x0, P0);
    if (!result) {
        return make_error<KalmanFilter>(
            result.error().code,
            result.error().message,
            "create_pairs_trading_filter"
        );
    }

    return Result<KalmanFilter>(std::move(filter));
}

// Helper: Create adaptive beta filter
Result<KalmanFilter> create_adaptive_beta_filter(
    double process_noise,
    double observation_noise)
{
    // State: [beta]
    // Observation: y (with x as known input)

    KalmanFilter filter(1, 1);

    // State transition (random walk for beta)
    Eigen::MatrixXd F(1, 1);
    F << 1.0;

    // Observation matrix (identity - will be set dynamically with x value)
    Eigen::MatrixXd H(1, 1);
    H << 1.0;

    // Process noise covariance
    Eigen::MatrixXd Q(1, 1);
    Q << process_noise * process_noise;

    // Observation noise covariance
    Eigen::MatrixXd R(1, 1);
    R << observation_noise * observation_noise;

    // Initial state
    Eigen::VectorXd x0(1);
    x0 << 0.0;

    // Initial covariance
    Eigen::MatrixXd P0(1, 1);
    P0 << 1.0;

    auto result = filter.initialize(F, H, Q, R, x0, P0);
    if (!result) {
        return make_error<KalmanFilter>(
            result.error().code,
            result.error().message,
            "create_adaptive_beta_filter"
        );
    }

    return Result<KalmanFilter>(std::move(filter));
}

} // namespace analysis
} // namespace trade_ngin
