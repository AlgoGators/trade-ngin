#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/kalman_filter.hpp"
#include <Eigen/Dense>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class KalmanFilterTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.state_dim = 2;
        config_.obs_dim = 1;
        config_.process_noise = 0.01;
        config_.measurement_noise = 0.1;
    }

    KalmanFilterConfig config_;
};

TEST_F(KalmanFilterTest, InitializeAndPredict) {
    KalmanFilter kf(config_);

    Eigen::VectorXd initial_state(2);
    initial_state << 0.0, 0.0;

    auto init_result = kf.initialize(initial_state);
    EXPECT_TRUE(init_result.is_ok());
    EXPECT_TRUE(kf.is_initialized());

    auto predict_result = kf.predict();
    ASSERT_TRUE(predict_result.is_ok());

    const auto& predicted_state = predict_result.value();
    EXPECT_EQ(predicted_state.size(), 2);
}

TEST_F(KalmanFilterTest, PredictUpdateCycle) {
    KalmanFilter kf(config_);

    // Set up constant velocity model
    Eigen::MatrixXd F(2, 2);
    F << 1.0, 1.0,
         0.0, 1.0;  // [position; velocity], dt=1
    ASSERT_TRUE(kf.set_transition_matrix(F).is_ok());

    Eigen::MatrixXd H(1, 2);
    H << 1.0, 0.0;  // Observe position only
    ASSERT_TRUE(kf.set_observation_matrix(H).is_ok());

    Eigen::VectorXd initial_state(2);
    initial_state << 0.0, 1.0;  // Position=0, velocity=1
    kf.initialize(initial_state);

    // Predict
    auto predict_result = kf.predict();
    ASSERT_TRUE(predict_result.is_ok());

    // Update with observation
    Eigen::VectorXd obs(1);
    obs << 1.0;  // Observe position = 1
    auto update_result = kf.update(obs);
    ASSERT_TRUE(update_result.is_ok());

    const auto& updated_state = update_result.value();
    EXPECT_EQ(updated_state.size(), 2);
}

TEST_F(KalmanFilterTest, NotInitializedError) {
    KalmanFilter kf(config_);

    auto predict_result = kf.predict();
    EXPECT_TRUE(predict_result.is_error());
    EXPECT_EQ(predict_result.error()->code(), trade_ngin::ErrorCode::NOT_INITIALIZED);
}

TEST(KalmanIllConditioned, NoNaNWithTinyMeasurementNoise) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    config.process_noise = 0.01;
    config.measurement_noise = 1e-12;

    KalmanFilter kf(config);

    Eigen::VectorXd initial_state(2);
    initial_state << 0.0, 1.0;
    auto init_result = kf.initialize(initial_state);
    ASSERT_TRUE(init_result.is_ok());

    Eigen::MatrixXd F(2, 2);
    F << 1.0, 1.0, 0.0, 1.0;
    ASSERT_TRUE(kf.set_transition_matrix(F).is_ok());

    Eigen::MatrixXd H(1, 2);
    H << 1.0, 0.0;
    ASSERT_TRUE(kf.set_observation_matrix(H).is_ok());

    // Run several predict-update cycles
    for (int i = 0; i < 20; ++i) {
        auto pred = kf.predict();
        ASSERT_TRUE(pred.is_ok());

        Eigen::VectorXd obs(1);
        obs << static_cast<double>(i);
        auto upd = kf.update(obs);
        ASSERT_TRUE(upd.is_ok());

        const auto& state = upd.value();
        for (int j = 0; j < state.size(); ++j) {
            EXPECT_FALSE(std::isnan(state(j))) << "NaN at step " << i << " dim " << j;
            EXPECT_FALSE(std::isinf(state(j))) << "Inf at step " << i << " dim " << j;
        }
    }
}

TEST(KalmanSetterTests, TransitionMatrixDimensionMismatch) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    Eigen::MatrixXd wrong_F(3, 3);
    wrong_F.setIdentity();
    auto result = kf.set_transition_matrix(wrong_F);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST(KalmanSetterTests, ObservationMatrixDimensionMismatch) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    Eigen::MatrixXd wrong_H(2, 2);
    wrong_H.setIdentity();
    auto result = kf.set_observation_matrix(wrong_H);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST(KalmanSetterTests, NaNTransitionMatrixRejected) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    Eigen::MatrixXd F(2, 2);
    F.setIdentity();
    F(0, 1) = std::numeric_limits<double>::quiet_NaN();
    auto result = kf.set_transition_matrix(F);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST(KalmanSetterTests, NonPositiveDefiniteQRejected) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    // Non-positive-definite matrix
    Eigen::MatrixXd Q(2, 2);
    Q << 1.0, 5.0,
         5.0, 1.0;  // eigenvalues: 6 and -4
    auto result = kf.set_process_noise(Q);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST(KalmanSetterTests, NonPositiveDefiniteRRejected) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 2;
    KalmanFilter kf(config);

    Eigen::MatrixXd R(2, 2);
    R << 1.0, 10.0,
         10.0, 1.0;
    auto result = kf.set_measurement_noise(R);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_ARGUMENT);
}

TEST(KalmanSetterTests, ValidSettersAccepted) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    Eigen::MatrixXd F(2, 2);
    F << 1.0, 1.0, 0.0, 1.0;
    EXPECT_TRUE(kf.set_transition_matrix(F).is_ok());

    Eigen::MatrixXd H(1, 2);
    H << 1.0, 0.0;
    EXPECT_TRUE(kf.set_observation_matrix(H).is_ok());

    Eigen::MatrixXd Q = Eigen::MatrixXd::Identity(2, 2) * 0.01;
    EXPECT_TRUE(kf.set_process_noise(Q).is_ok());

    Eigen::MatrixXd R(1, 1);
    R << 0.1;
    EXPECT_TRUE(kf.set_measurement_noise(R).is_ok());
}

TEST(ValidationTests, KalmanInitializeNaNRejected) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    Eigen::VectorXd state(2);
    state << 0.0, std::numeric_limits<double>::quiet_NaN();
    auto result = kf.initialize(state);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST(ValidationTests, KalmanUpdateNaNRejected) {
    KalmanFilterConfig config;
    config.state_dim = 2;
    config.obs_dim = 1;
    KalmanFilter kf(config);

    Eigen::VectorXd state(2);
    state << 0.0, 0.0;
    kf.initialize(state);
    kf.predict();

    Eigen::VectorXd obs(1);
    obs << std::numeric_limits<double>::quiet_NaN();
    auto result = kf.update(obs);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
