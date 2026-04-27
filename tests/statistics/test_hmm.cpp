#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/hmm.hpp"
#include <Eigen/Dense>
#include <random>
#include <cmath>
#include <limits>

using namespace trade_ngin::statistics;

class HMMTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_.n_states = 2;
        config_.max_iterations = 50;
        config_.tolerance = 1e-4;

        // Generate observations from 2-state HMM
        std::mt19937 gen(42);
        std::normal_distribution<> state0(0.0, 0.5);
        std::normal_distribution<> state1(3.0, 0.5);

        int n = 100;
        observations_ = Eigen::MatrixXd(n, 1);

        int current_state = 0;
        for (int i = 0; i < n; ++i) {
            // Simple state transitions
            if (std::uniform_real_distribution<>(0, 1)(gen) < 0.1) {
                current_state = 1 - current_state;  // Switch state
            }

            if (current_state == 0) {
                observations_(i, 0) = state0(gen);
            } else {
                observations_(i, 0) = state1(gen);
            }
        }
    }

    HMMConfig config_;
    Eigen::MatrixXd observations_;
};

TEST_F(HMMTest, FitAndDecode) {
    HMM hmm(config_);

    auto fit_result = hmm.fit(observations_);
    EXPECT_TRUE(fit_result.is_ok());
    EXPECT_TRUE(hmm.is_initialized());

    auto decode_result = hmm.decode(observations_);
    ASSERT_TRUE(decode_result.is_ok());

    const auto& states = decode_result.value();
    EXPECT_EQ(states.size(), observations_.rows());

    // States should be 0 or 1
    for (int state : states) {
        EXPECT_TRUE(state == 0 || state == 1);
    }
}

TEST_F(HMMTest, InitializeAndUpdate) {
    HMM hmm(config_);

    Eigen::VectorXd initial_probs(2);
    initial_probs << 0.5, 0.5;

    auto init_result = hmm.initialize(initial_probs);
    EXPECT_TRUE(init_result.is_ok());

    Eigen::VectorXd obs(1);
    obs << 1.0;

    auto update_result = hmm.update(obs);
    ASSERT_TRUE(update_result.is_ok());

    const auto& state_probs = update_result.value();
    EXPECT_EQ(state_probs.size(), 2);

    // Probabilities should sum to 1
    EXPECT_NEAR(state_probs.sum(), 1.0, 1e-6);
}

TEST_F(HMMTest, PredictNextState) {
    HMM hmm(config_);
    hmm.fit(observations_);

    auto predict_result = hmm.predict();
    ASSERT_TRUE(predict_result.is_ok());

    const auto& next_probs = predict_result.value();
    EXPECT_EQ(next_probs.size(), 2);
    EXPECT_NEAR(next_probs.sum(), 1.0, 1e-6);
}

TEST_F(HMMTest, InsufficientDataError) {
    HMM hmm(config_);

    Eigen::MatrixXd small_obs(5, 1);
    auto result = hmm.fit(small_obs);
    EXPECT_TRUE(result.is_error());
}

TEST(HMMLongSequence, FitAndDecodeT500) {
    // Generate T=500 observation sequence from 2 well-separated states
    std::mt19937 gen(123);
    std::normal_distribution<> state0(0.0, 0.5);
    std::normal_distribution<> state1(5.0, 0.5);

    int T = 500;
    Eigen::MatrixXd obs(T, 1);
    std::vector<int> true_states(T);

    int current_state = 0;
    for (int i = 0; i < T; ++i) {
        if (std::uniform_real_distribution<>(0, 1)(gen) < 0.05) {
            current_state = 1 - current_state;
        }
        true_states[i] = current_state;
        obs(i, 0) = (current_state == 0) ? state0(gen) : state1(gen);
    }

    HMMConfig config;
    config.n_states = 2;
    config.max_iterations = 100;
    config.tolerance = 1e-4;
    // Default init_random=true seeds the transition matrix from global rand(),
    // which other tests in the suite advance non-deterministically. Force the
    // deterministic uniform init so this test is order-independent.
    config.init_random = false;

    HMM hmm(config);
    auto fit_result = hmm.fit(obs);
    ASSERT_TRUE(fit_result.is_ok());

    auto decode_result = hmm.decode(obs);
    ASSERT_TRUE(decode_result.is_ok());

    const auto& decoded = decode_result.value();
    EXPECT_EQ(static_cast<int>(decoded.size()), T);

    // Check states are valid
    for (int s : decoded) {
        EXPECT_TRUE(s == 0 || s == 1);
    }

    // Decoded states should mostly agree with true states (allowing label swap)
    int agree = 0, disagree = 0;
    for (int i = 0; i < T; ++i) {
        if (decoded[i] == true_states[i]) agree++;
        else disagree++;
    }
    // Either direct or swapped labels should have >80% accuracy
    int best_match = std::max(agree, disagree);
    EXPECT_GT(best_match, T * 0.80) << "Decoded states don't match true states well enough";
}

TEST(HMMIllConditioned, NearlyIdenticalObservations) {
    // Observations very close together — should not crash
    int T = 50;
    Eigen::MatrixXd obs(T, 1);
    for (int i = 0; i < T; ++i) {
        obs(i, 0) = 1.0 + 1e-8 * i;  // Nearly identical
    }

    HMMConfig config;
    config.n_states = 2;
    config.max_iterations = 20;

    HMM hmm(config);
    auto fit_result = hmm.fit(obs);
    // Should complete without crashing; may or may not converge well
    EXPECT_TRUE(fit_result.is_ok());
}

TEST(ValidationTests, HMMInitializeNaNRejected) {
    HMMConfig config;
    config.n_states = 2;
    HMM hmm(config);

    Eigen::VectorXd state(2);
    state << 0.5, std::numeric_limits<double>::quiet_NaN();
    auto result = hmm.initialize(state);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST(ValidationTests, HMMFitNaNRejected) {
    HMMConfig config;
    config.n_states = 2;
    HMM hmm(config);

    Eigen::MatrixXd obs(20, 1);
    obs.setOnes();
    obs(10, 0) = std::numeric_limits<double>::quiet_NaN();
    auto result = hmm.fit(obs);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}
