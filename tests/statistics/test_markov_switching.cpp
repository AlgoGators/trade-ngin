#include <gtest/gtest.h>
#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include <random>
#include <cmath>
#include <limits>
#include <algorithm>

using namespace trade_ngin::statistics;

class MarkovSwitchingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Generate 2-state regime switching data
        std::mt19937 gen(42);
        std::normal_distribution<> state0(0.0, 0.5);
        std::normal_distribution<> state1(3.0, 0.5);

        int n = 300;
        data_.resize(n);
        true_states_.resize(n);

        int current_state = 0;
        for (int i = 0; i < n; ++i) {
            if (std::uniform_real_distribution<>(0, 1)(gen) < 0.05) {
                current_state = 1 - current_state;
            }
            true_states_[i] = current_state;
            data_[i] = (current_state == 0) ? state0(gen) : state1(gen);
        }
    }

    std::vector<double> data_;
    std::vector<int> true_states_;
};

TEST_F(MarkovSwitchingTest, FitTwoState) {
    MarkovSwitchingConfig config;
    config.n_states = 2;
    config.max_iterations = 100;
    config.tolerance = 1e-4;

    MarkovSwitching ms(config);
    auto result = ms.fit(data_);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(ms.is_initialized());

    const auto& res = result.value();
    EXPECT_EQ(res.state_means.size(), 2);
    EXPECT_EQ(res.state_variances.size(), 2);
    EXPECT_EQ(res.transition_matrix.rows(), 2);
    EXPECT_EQ(res.transition_matrix.cols(), 2);

    // Means should be separated
    double mean_diff = std::abs(res.state_means(0) - res.state_means(1));
    EXPECT_GT(mean_diff, 1.0);
}

TEST_F(MarkovSwitchingTest, DecodeAccuracy) {
    MarkovSwitchingConfig config;
    config.n_states = 2;
    config.max_iterations = 100;

    MarkovSwitching ms(config);
    auto fit_result = ms.fit(data_);
    ASSERT_TRUE(fit_result.is_ok());

    auto decode_result = ms.decode();
    ASSERT_TRUE(decode_result.is_ok());

    const auto& decoded = decode_result.value();
    EXPECT_EQ(decoded.size(), data_.size());

    // Check accuracy (allowing label swap)
    int agree = 0, disagree = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
        if (decoded[i] == true_states_[i]) agree++;
        else disagree++;
    }
    int best_match = std::max(agree, disagree);
    EXPECT_GT(best_match, static_cast<int>(data_.size()) * 0.80)
        << "Decoded states don't match true states well enough";
}

TEST_F(MarkovSwitchingTest, TransitionMatrix) {
    MarkovSwitchingConfig config;
    config.n_states = 2;

    MarkovSwitching ms(config);
    ms.fit(data_);

    const auto& tm = ms.get_transition_matrix();

    // Rows should sum to ~1
    for (int i = 0; i < 2; ++i) {
        EXPECT_NEAR(tm.row(i).sum(), 1.0, 1e-6);
    }

    // Diagonal should be high (persistent states)
    EXPECT_GT(tm(0, 0), 0.7);
    EXPECT_GT(tm(1, 1), 0.7);
}

TEST_F(MarkovSwitchingTest, ThreeStateRegime) {
    // Generate 3-state data
    std::mt19937 gen(123);
    std::normal_distribution<> s0(-2.0, 0.3);
    std::normal_distribution<> s1(0.0, 0.3);
    std::normal_distribution<> s2(2.0, 0.3);

    int n = 600;
    std::vector<double> data(n);
    int state = 0;
    for (int i = 0; i < n; ++i) {
        double r = std::uniform_real_distribution<>(0, 1)(gen);
        if (r < 0.03) state = (state + 1) % 3;
        else if (r < 0.06) state = (state + 2) % 3;

        if (state == 0) data[i] = s0(gen);
        else if (state == 1) data[i] = s1(gen);
        else data[i] = s2(gen);
    }

    MarkovSwitchingConfig config;
    config.n_states = 3;
    config.max_iterations = 150;

    MarkovSwitching ms(config);
    auto result = ms.fit(data);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    EXPECT_EQ(res.state_means.size(), 3);

    // Sort means and check separation
    std::vector<double> means(3);
    for (int i = 0; i < 3; ++i) means[i] = res.state_means(i);
    std::sort(means.begin(), means.end());
    EXPECT_GT(means[2] - means[0], 2.0);
}

TEST_F(MarkovSwitchingTest, SmoothedProbabilities) {
    MarkovSwitchingConfig config;
    config.n_states = 2;

    MarkovSwitching ms(config);
    auto result = ms.fit(data_);
    ASSERT_TRUE(result.is_ok());

    const auto& res = result.value();
    int T = static_cast<int>(data_.size());

    EXPECT_EQ(res.smoothed_probabilities.rows(), T);
    EXPECT_EQ(res.smoothed_probabilities.cols(), 2);

    // Each row should sum to ~1
    for (int t = 0; t < T; ++t) {
        EXPECT_NEAR(res.smoothed_probabilities.row(t).sum(), 1.0, 0.01);
    }
}

TEST_F(MarkovSwitchingTest, StateEstimatorInterface) {
    MarkovSwitchingConfig config;
    config.n_states = 2;

    MarkovSwitching ms(config);
    ms.fit(data_);

    auto predict_result = ms.predict();
    ASSERT_TRUE(predict_result.is_ok());
    EXPECT_EQ(predict_result.value().size(), 2);
    EXPECT_NEAR(predict_result.value().sum(), 1.0, 1e-6);

    Eigen::VectorXd obs(1);
    obs << 1.0;
    auto update_result = ms.update(obs);
    ASSERT_TRUE(update_result.is_ok());
    EXPECT_NEAR(update_result.value().sum(), 1.0, 1e-6);
}

TEST_F(MarkovSwitchingTest, InsufficientDataError) {
    MarkovSwitchingConfig config;
    MarkovSwitching ms(config);

    std::vector<double> small = {1.0, 2.0, 3.0};
    auto result = ms.fit(small);
    EXPECT_TRUE(result.is_error());
}

TEST_F(MarkovSwitchingTest, NaNRejected) {
    std::vector<double> data(100, 1.0);
    data[50] = std::numeric_limits<double>::quiet_NaN();

    MarkovSwitchingConfig config;
    MarkovSwitching ms(config);
    auto result = ms.fit(data);
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error()->code(), trade_ngin::ErrorCode::INVALID_DATA);
}

TEST_F(MarkovSwitchingTest, DecodeBeforeFitError) {
    MarkovSwitchingConfig config;
    MarkovSwitching ms(config);

    auto result = ms.decode();
    EXPECT_TRUE(result.is_error());
}

TEST_F(MarkovSwitchingTest, InitializeManually) {
    MarkovSwitchingConfig config;
    config.n_states = 2;
    MarkovSwitching ms(config);

    Eigen::VectorXd probs(2);
    probs << 0.7, 0.3;

    auto init_result = ms.initialize(probs);
    EXPECT_TRUE(init_result.is_ok());
    EXPECT_TRUE(ms.is_initialized());

    auto state = ms.get_state();
    ASSERT_TRUE(state.is_ok());
    EXPECT_NEAR(state.value().sum(), 1.0, 1e-6);
}
