#include "trade_ngin/statistics/state_estimation/kalman_filter.hpp"
#include "trade_ngin/core/logger.hpp"
#include <string>

namespace trade_ngin {
namespace statistics {

KalmanFilter::KalmanFilter(KalmanFilterConfig config)
    : config_(config) {

    // Initialize matrices with default values
    F_ = Eigen::MatrixXd::Identity(config.state_dim, config.state_dim);
    H_ = Eigen::MatrixXd::Identity(config.obs_dim, config.state_dim);
    Q_ = Eigen::MatrixXd::Identity(config.state_dim, config.state_dim) * config.process_noise;
    R_ = Eigen::MatrixXd::Identity(config.obs_dim, config.obs_dim) * config.measurement_noise;
}

Result<void> KalmanFilter::initialize(const Eigen::VectorXd& initial_state) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initial_state.size() != config_.state_dim) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Initial state dimension mismatch",
            "KalmanFilter"
        );
    }

    if (!initial_state.allFinite()) {
        return make_error<void>(
            ErrorCode::INVALID_DATA,
            "NaN/Inf detected in initial state",
            "KalmanFilter"
        );
    }

    DEBUG("[KalmanFilter::initialize] entry: state_dim=" << config_.state_dim);

    x_ = initial_state;
    P_ = Eigen::MatrixXd::Identity(config_.state_dim, config_.state_dim);
    initialized_ = true;

    return Result<void>();
}

Result<Eigen::VectorXd> KalmanFilter::predict() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "Kalman filter has not been initialized",
            "KalmanFilter"
        );
    }

    // Predict state: x_k|k-1 = F * x_k-1|k-1
    x_ = F_ * x_;

    // Predict covariance: P_k|k-1 = F * P_k-1|k-1 * F^T + Q
    P_ = F_ * P_ * F_.transpose() + Q_;

    return Result<Eigen::VectorXd>(x_);
}

Result<Eigen::VectorXd> KalmanFilter::update(const Eigen::VectorXd& observation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "Kalman filter has not been initialized",
            "KalmanFilter"
        );
    }

    if (observation.size() != config_.obs_dim) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::INVALID_ARGUMENT,
            "Observation dimension mismatch",
            "KalmanFilter"
        );
    }

    if (!observation.allFinite()) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::INVALID_DATA,
            "NaN/Inf detected in observation",
            "KalmanFilter"
        );
    }

    DEBUG("[KalmanFilter::update] entry: obs_dim=" << observation.size()
          << " state_dim=" << x_.size());

    // Innovation: y = z - H * x_k|k-1
    Eigen::VectorXd y = observation - H_ * x_;

    // Innovation covariance: S = H * P_k|k-1 * H^T + R
    Eigen::MatrixXd S = H_ * P_ * H_.transpose() + R_;

    // Check condition number of S
    {
        Eigen::JacobiSVD<Eigen::MatrixXd> svd(S);
        double cond = svd.singularValues()(0) /
                      svd.singularValues()(svd.singularValues().size() - 1);
        if (cond > 1e10) {
            WARN("[KalmanFilter::update] ill-conditioned innovation covariance S: cond=" << cond);
        }
    }

    // Kalman gain: K = P*H' * S^{-1} = (S^{-1} * (P*H')')' using Cholesky
    Eigen::MatrixXd PH_t = P_ * H_.transpose();
    Eigen::MatrixXd K;
    Eigen::LLT<Eigen::MatrixXd> llt_S(S);
    if (llt_S.info() == Eigen::Success) {
        K = llt_S.solve(PH_t.transpose()).transpose();
    } else {
        K = Eigen::LDLT<Eigen::MatrixXd>(S).solve(PH_t.transpose()).transpose();
    }

    // Update state: x_k|k = x_k|k-1 + K * y
    x_ = x_ + K * y;

    // Update covariance: P_k|k = (I - K * H) * P_k|k-1
    Eigen::MatrixXd I = Eigen::MatrixXd::Identity(config_.state_dim, config_.state_dim);
    P_ = (I - K * H_) * P_;

    return Result<Eigen::VectorXd>(x_);
}

Result<Eigen::VectorXd> KalmanFilter::get_state() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return make_error<Eigen::VectorXd>(
            ErrorCode::NOT_INITIALIZED,
            "Kalman filter has not been initialized",
            "KalmanFilter"
        );
    }

    return Result<Eigen::VectorXd>(x_);
}

Result<void> KalmanFilter::set_transition_matrix(const Eigen::MatrixXd& F) {
    if (F.rows() != config_.state_dim || F.cols() != config_.state_dim) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Transition matrix dimension mismatch: expected " +
            std::to_string(config_.state_dim) + "x" + std::to_string(config_.state_dim) +
            ", got " + std::to_string(F.rows()) + "x" + std::to_string(F.cols()),
            "KalmanFilter"
        );
    }
    if (!F.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA, "NaN/Inf detected in transition matrix", "KalmanFilter");
    }
    F_ = F;
    return Result<void>();
}

Result<void> KalmanFilter::set_observation_matrix(const Eigen::MatrixXd& H) {
    if (H.rows() != config_.obs_dim || H.cols() != config_.state_dim) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Observation matrix dimension mismatch: expected " +
            std::to_string(config_.obs_dim) + "x" + std::to_string(config_.state_dim) +
            ", got " + std::to_string(H.rows()) + "x" + std::to_string(H.cols()),
            "KalmanFilter"
        );
    }
    if (!H.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA, "NaN/Inf detected in observation matrix", "KalmanFilter");
    }
    H_ = H;
    return Result<void>();
}

Result<void> KalmanFilter::set_process_noise(const Eigen::MatrixXd& Q) {
    if (Q.rows() != config_.state_dim || Q.cols() != config_.state_dim) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Process noise dimension mismatch: expected " +
            std::to_string(config_.state_dim) + "x" + std::to_string(config_.state_dim) +
            ", got " + std::to_string(Q.rows()) + "x" + std::to_string(Q.cols()),
            "KalmanFilter"
        );
    }
    if (!Q.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA, "NaN/Inf detected in process noise matrix", "KalmanFilter");
    }
    Eigen::LLT<Eigen::MatrixXd> llt(Q);
    if (llt.info() != Eigen::Success) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Process noise matrix Q is not positive-definite",
            "KalmanFilter"
        );
    }
    Q_ = Q;
    return Result<void>();
}

Result<void> KalmanFilter::set_measurement_noise(const Eigen::MatrixXd& R) {
    if (R.rows() != config_.obs_dim || R.cols() != config_.obs_dim) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Measurement noise dimension mismatch: expected " +
            std::to_string(config_.obs_dim) + "x" + std::to_string(config_.obs_dim) +
            ", got " + std::to_string(R.rows()) + "x" + std::to_string(R.cols()),
            "KalmanFilter"
        );
    }
    if (!R.allFinite()) {
        return make_error<void>(ErrorCode::INVALID_DATA, "NaN/Inf detected in measurement noise matrix", "KalmanFilter");
    }
    Eigen::LLT<Eigen::MatrixXd> llt(R);
    if (llt.info() != Eigen::Success) {
        return make_error<void>(
            ErrorCode::INVALID_ARGUMENT,
            "Measurement noise matrix R is not positive-definite",
            "KalmanFilter"
        );
    }
    R_ = R;
    return Result<void>();
}

} // namespace statistics
} // namespace trade_ngin
