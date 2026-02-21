#include "trade_ngin/statistics/state_estimation/extended_kalman_filter.hpp"
#include "trade_ngin/core/logger.hpp"

namespace trade_ngin {
namespace statistics {

ExtendedKalmanFilter::ExtendedKalmanFilter(ExtendedKalmanFilterConfig config)
    : config_(config) {
    Q_ = Eigen::MatrixXd::Identity(config.state_dim, config.state_dim) * 0.01;
    R_ = Eigen::MatrixXd::Identity(config.obs_dim, config.obs_dim) * 0.1;
}

Result<void> ExtendedKalmanFilter::initialize(const Eigen::VectorXd& initial_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initial_state.size() != config_.state_dim) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Initial state dimension mismatch", "ExtendedKalmanFilter");
    }

    if (!initial_state.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
            "NaN/Inf detected in initial state", "ExtendedKalmanFilter");
    }

    DEBUG("[ExtendedKalmanFilter::initialize] state_dim=" << config_.state_dim);

    x_ = initial_state;
    P_ = Eigen::MatrixXd::Identity(config_.state_dim, config_.state_dim);
    initialized_ = true;

    return Result<void>();
}

Result<Eigen::VectorXd> ExtendedKalmanFilter::predict() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "Extended Kalman filter has not been initialized", "ExtendedKalmanFilter");
    }

    if (transition_set_ && f_ && F_jacobian_) {
        // Nonlinear prediction
        Eigen::MatrixXd F = F_jacobian_(x_);
        x_ = f_(x_);
        P_ = F * P_ * F.transpose() + Q_;
    } else {
        // Default: identity transition (x_{k+1} = x_k)
        P_ = P_ + Q_;
    }

    return Result<Eigen::VectorXd>(x_);
}

Result<Eigen::VectorXd> ExtendedKalmanFilter::update(const Eigen::VectorXd& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "Extended Kalman filter has not been initialized", "ExtendedKalmanFilter");
    }

    if (observation.size() != config_.obs_dim) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_ARGUMENT,
            "Observation dimension mismatch", "ExtendedKalmanFilter");
    }

    if (!observation.allFinite()) {
        return make_error<Eigen::VectorXd>(ErrorCode::INVALID_DATA,
            "NaN/Inf detected in observation", "ExtendedKalmanFilter");
    }

    Eigen::VectorXd h_x;
    Eigen::MatrixXd H;

    if (observation_set_ && h_ && H_jacobian_) {
        h_x = h_(x_);
        H = H_jacobian_(x_);
    } else {
        // Default: identity observation (z = x)
        H = Eigen::MatrixXd::Identity(config_.obs_dim, config_.state_dim);
        h_x = H * x_;
    }

    // Innovation
    Eigen::VectorXd y = observation - h_x;

    // Innovation covariance
    Eigen::MatrixXd S = H * P_ * H.transpose() + R_;

    // Kalman gain via Cholesky
    Eigen::MatrixXd PH_t = P_ * H.transpose();
    Eigen::MatrixXd K;
    Eigen::LLT<Eigen::MatrixXd> llt_S(S);
    if (llt_S.info() == Eigen::Success) {
        K = llt_S.solve(PH_t.transpose()).transpose();
    } else {
        K = Eigen::LDLT<Eigen::MatrixXd>(S).solve(PH_t.transpose()).transpose();
    }

    // Update state and covariance
    x_ = x_ + K * y;
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(config_.state_dim, config_.state_dim);
    P_ = (I - K * H) * P_;

    return Result<Eigen::VectorXd>(x_);
}

Result<Eigen::VectorXd> ExtendedKalmanFilter::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(ErrorCode::NOT_INITIALIZED,
            "Extended Kalman filter has not been initialized", "ExtendedKalmanFilter");
    }

    return Result<Eigen::VectorXd>(x_);
}

Result<void> ExtendedKalmanFilter::set_transition_function(TransitionFunc f, JacobianFunc F_jacobian) {
    if (!f || !F_jacobian) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Transition function and Jacobian must not be null", "ExtendedKalmanFilter");
    }
    f_ = std::move(f);
    F_jacobian_ = std::move(F_jacobian);
    transition_set_ = true;
    return Result<void>();
}

Result<void> ExtendedKalmanFilter::set_observation_function(TransitionFunc h, JacobianFunc H_jacobian) {
    if (!h || !H_jacobian) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Observation function and Jacobian must not be null", "ExtendedKalmanFilter");
    }
    h_ = std::move(h);
    H_jacobian_ = std::move(H_jacobian);
    observation_set_ = true;
    return Result<void>();
}

Result<void> ExtendedKalmanFilter::set_process_noise(const Eigen::MatrixXd& Q) {
    if (Q.rows() != config_.state_dim || Q.cols() != config_.state_dim) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Process noise dimension mismatch", "ExtendedKalmanFilter");
    }
    if (!Q.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
            "NaN/Inf detected in process noise matrix", "ExtendedKalmanFilter");
    }
    Q_ = Q;
    return Result<void>();
}

Result<void> ExtendedKalmanFilter::set_measurement_noise(const Eigen::MatrixXd& R) {
    if (R.rows() != config_.obs_dim || R.cols() != config_.obs_dim) {
        return make_error<void>(ErrorCode::INVALID_ARGUMENT,
            "Measurement noise dimension mismatch", "ExtendedKalmanFilter");
    }
    if (!R.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA,
            "NaN/Inf detected in measurement noise matrix", "ExtendedKalmanFilter");
    }
    R_ = R;
    return Result<void>();
}

} // namespace statistics
} // namespace trade_ngin
