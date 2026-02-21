#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/extended_kalman_filter.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class ExtendedKalmanFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.state_dim = 2;
        config_.obs_dim = 1;
    }

    ExtendedKalmanFilterConfig config_;
};

TEST_F(ExtendedKalmanFilterTest, InitializeAndPredict) {
    ExtendedKalmanFilter ekf(config_);

    Eigen::VectorXd state(2);
    state << 0.0, 1.0;

    auto init = ekf.initialize(state);
    EXPECT_TRUE(init.is_ok());
    EXPECT_TRUE(ekf.is_initialized());

    auto pred = ekf.predict();
    ASSERT_TRUE(pred.is_ok());
    EXPECT_EQ(pred.value().size(), 2);
}

TEST_F(ExtendedKalmanFilterTest, LinearTransition) {
    // Use EKF with linear functions (should behave like regular KF)
    ExtendedKalmanFilter ekf(config_);

    // Constant velocity model: x = [position, velocity]
    auto f = [](const Eigen::VectorXd& x) -> Eigen::VectorXd {
        Eigen::VectorXd xn(2);
        xn(0) = x(0) + x(1);  // pos += vel
        xn(1) = x(1);          // vel constant
        return xn;
    };

    auto F_jac = [](const Eigen::VectorXd&) -> Eigen::MatrixXd {
        Eigen::MatrixXd F(2, 2);
        F << 1.0, 1.0,
             0.0, 1.0;
        return F;
    };

    auto h = [](const Eigen::VectorXd& x) -> Eigen::VectorXd {
        Eigen::VectorXd z(1);
        z(0) = x(0);  // Observe position only
        return z;
    };

    auto H_jac = [](const Eigen::VectorXd&) -> Eigen::MatrixXd {
        Eigen::MatrixXd H(1, 2);
        H << 1.0, 0.0;
        return H;
    };

    ASSERT_TRUE(ekf.set_transition_function(f, F_jac).is_ok());
    ASSERT_TRUE(ekf.set_observation_function(h, H_jac).is_ok());

    Eigen::VectorXd state(2);
    state << 0.0, 1.0;
    ekf.initialize(state);

    // Run predict-update cycles
    for (int i = 0; i < 10; ++i) {
        ekf.predict();

        Eigen::VectorXd obs(1);
        obs << static_cast<double>(i + 1) + 0.1;  // Noisy position
        auto upd = ekf.update(obs);
        ASSERT_TRUE(upd.is_ok());

        const auto& s = upd.value();
        EXPECT_TRUE(s.allFinite());
    }

    // After tracking, position estimate should be near true value
    auto final_state = ekf.get_state();
    ASSERT_TRUE(final_state.is_ok());
    EXPECT_NEAR(final_state.value()(0), 10.0, 2.0);
}

TEST_F(ExtendedKalmanFilterTest, NonlinearTransition) {
    // Pendulum-like nonlinear dynamics: θ' = ω, ω' = -sin(θ)
    ExtendedKalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;

    ExtendedKalmanFilter ekf(config);

    double dt = 0.1;
    auto f = [dt](const Eigen::VectorXd& x) -> Eigen::VectorXd {
        Eigen::VectorXd xn(2);
        xn(0) = x(0) + dt * x(1);
        xn(1) = x(1) - dt * std::sin(x(0));
        return xn;
    };

    auto F_jac = [dt](const Eigen::VectorXd& x) -> Eigen::MatrixXd {
        Eigen::MatrixXd F(2, 2);
        F << 1.0, dt,
             -dt * std::cos(x(0)), 1.0;
        return F;
    };

    auto h = [](const Eigen::VectorXd& x) -> Eigen::VectorXd {
        Eigen::VectorXd z(1);
        z(0) = x(0);  // Observe angle
        return z;
    };

    auto H_jac = [](const Eigen::VectorXd&) -> Eigen::MatrixXd {
        Eigen::MatrixXd H(1, 2);
        H << 1.0, 0.0;
        return H;
    };

    ASSERT_TRUE(ekf.set_transition_function(f, F_jac).is_ok());
    ASSERT_TRUE(ekf.set_observation_function(h, H_jac).is_ok());

    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(2, 2) * 0.001;
    Eigen::MatrixXd R(1, 1);
    R << 0.01;
    ASSERT_TRUE(ekf.set_process_noise(Q).is_ok());
    ASSERT_TRUE(ekf.set_measurement_noise(R).is_ok());

    Eigen::VectorXd state(2);
    state << 0.5, 0.0;  // Initial angle, zero velocity
    ekf.initialize(state);

    // Run cycles - should not diverge
    for (int i = 0; i < 50; ++i) {
        auto pred = ekf.predict();
        ASSERT_TRUE(pred.is_ok());

        Eigen::VectorXd obs(1);
        obs << 0.5 * std::cos(0.1 * i);  // Simulated observation
        auto upd = ekf.update(obs);
        ASSERT_TRUE(upd.is_ok());
        EXPECT_TRUE(upd.value().allFinite());
    }
}

TEST_F(ExtendedKalmanFilterTest, DefaultIdentityBehavior) {
    // Without setting functions, should default to identity
    ExtendedKalmanFilter ekf(config_);

    Eigen::VectorXd state(2);
    state << 1.0, 2.0;
    ekf.initialize(state);

    ekf.predict();
    auto s = ekf.get_state();
    ASSERT_TRUE(s.is_ok());

    // Identity transition preserves state
    EXPECT_NEAR(s.value()(0), 1.0, 0.5);
    EXPECT_NEAR(s.value()(1), 2.0, 0.5);
}

TEST_F(ExtendedKalmanFilterTest, ProcessNoiseSetters) {
    ExtendedKalmanFilter ekf(config_);

    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(2, 2) * 0.05;
    EXPECT_TRUE(ekf.set_process_noise(Q).is_ok());

    Eigen::MatrixXd R(1, 1);
    R << 0.1;
    EXPECT_TRUE(ekf.set_measurement_noise(R).is_ok());
}

TEST_F(ExtendedKalmanFilterTest, DimensionMismatchErrors) {
    ExtendedKalmanFilter ekf(config_);

    Eigen::MatrixXd Q_wrong(3, 3);
    Q_wrong.setIdentity();
    EXPECT_TRUE(ekf.set_process_noise(Q_wrong).is_error());

    Eigen::MatrixXd R_wrong(2, 2);
    R_wrong.setIdentity();
    EXPECT_TRUE(ekf.set_measurement_noise(R_wrong).is_error());
}

TEST_F(ExtendedKalmanFilterTest, NotInitializedError) {
    ExtendedKalmanFilter ekf(config_);

    auto pred = ekf.predict();
    EXPECT_TRUE(pred.is_error());
    EXPECT_EQ(pred.error()->code(), trade_ngin::ErrorCode::NOT_INITIALIZED);
}

TEST_F(ExtendedKalmanFilterTest, NaNInitialStateRejected) {
    ExtendedKalmanFilter ekf(config_);

    Eigen::VectorXd state(2);
    state << 0.0, std::numeric_limits<double>::quiet_NaN();
    auto result = ekf.initialize(state);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(ExtendedKalmanFilterTest, NaNObservationRejected) {
    ExtendedKalmanFilter ekf(config_);

    Eigen::VectorXd state(2);
    state << 0.0, 0.0;
    ekf.initialize(state);
    ekf.predict();

    Eigen::VectorXd obs(1);
    obs << std::numeric_limits<double>::quiet_NaN();
    auto result = ekf.update(obs);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(ExtendedKalmanFilterTest, StateCovariance) {
    ExtendedKalmanFilter ekf(config_);

    Eigen::VectorXd state(2);
    state << 0.0, 0.0;
    ekf.initialize(state);

    const auto& P = ekf.get_state_covariance();
    EXPECT_EQ(P.rows(), 2);
    EXPECT_EQ(P.cols(), 2);
    EXPECT_GT(P(0, 0), 0.0);
}
