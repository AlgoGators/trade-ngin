#include <gtest/gtest.h>
#include "trade_ngin/statistics/volatility/garch.hpp"
#include "trade_ngin/statistics/volatility/egarch.hpp"
#include "trade_ngin/statistics/volatility/gjr_garch.hpp"
#include "trade_ngin/statistics/state_estimation/hmm.hpp"
#include "trade_ngin/statistics/state_estimation/markov_switching.hpp"
#include "trade_ngin/statistics/regression/lasso_regression.hpp"
#include <random>
#include <cmath>

using namespace trade_ngin::statistics;

class ConvergenceMonitoringTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::mt19937 gen(42);
        std::normal_distribution<> d(0.0, 1.0);

        // Generate GARCH-like returns
        returns_.resize(200);
        double sigma = 0.01;
        for (size_t i = 0; i < returns_.size(); ++i) {
            double z = d(gen);
            returns_[i] = sigma * z;
            sigma = std::sqrt(0.00001 + 0.1 * returns_[i] * returns_[i] + 0.85 * sigma * sigma);
        }

        // Generate regime-switching data for HMM/MarkovSwitching
        regime_data_.resize(100, 1);
        for (int i = 0; i < 50; ++i) {
            regime_data_(i, 0) = d(gen) * 0.5 + 1.0;   // State 0: mean=1
        }
        for (int i = 50; i < 100; ++i) {
            regime_data_(i, 0) = d(gen) * 0.5 - 1.0;  // State 1: mean=-1
        }

        // Generate data for MarkovSwitching (vector form)
        ms_data_.resize(100);
        for (int i = 0; i < 50; ++i) ms_data_[i] = d(gen) * 0.5 + 1.0;
        for (int i = 50; i < 100; ++i) ms_data_[i] = d(gen) * 0.5 - 1.0;
    }

    std::vector<double> returns_;
    Eigen::MatrixXd regime_data_;
    std::vector<double> ms_data_;
};

TEST_F(ConvergenceMonitoringTest, GARCHFitWithDiagnostics) {
    GARCHConfig config;
    GARCH garch(config);

    auto result = garch.fit_with_diagnostics(returns_);
    ASSERT_TRUE(result.is_ok());

    const auto& info = result.value();
    EXPECT_GT(info.iterations, 0);
    EXPECT_FALSE(info.objective_history.empty());
    EXPECT_FALSE(info.termination_reason.empty());
}

TEST_F(ConvergenceMonitoringTest, GARCHGetConvergenceInfo) {
    GARCHConfig config;
    GARCH garch(config);

    // Before fitting, convergence info should be default
    const auto& info_before = garch.get_convergence_info();
    EXPECT_EQ(info_before.iterations, 0);
    EXPECT_FALSE(info_before.converged);

    garch.fit(returns_);

    const auto& info_after = garch.get_convergence_info();
    EXPECT_GT(info_after.iterations, 0);
    EXPECT_FALSE(info_after.objective_history.empty());
}

TEST_F(ConvergenceMonitoringTest, HMMFitWithDiagnosticsTracksObjectiveHistory) {
    HMMConfig config;
    config.n_states = 2;
    config.max_iterations = 50;
    HMM hmm(config);

    auto result = hmm.fit_with_diagnostics(regime_data_);
    ASSERT_TRUE(result.is_ok());

    const auto& info = result.value();
    EXPECT_GT(info.iterations, 0);
    EXPECT_FALSE(info.objective_history.empty());
    // LL should generally increase over iterations
    if (info.objective_history.size() >= 2) {
        EXPECT_GE(info.objective_history.back(), info.objective_history.front() - 1.0);
    }
}

TEST_F(ConvergenceMonitoringTest, LassoConvergenceInfoTracksIterations) {
    LassoRegressionConfig config;
    config.alpha = 0.01;
    LassoRegression lasso(config);

    // Create simple regression data
    Eigen::MatrixXd X(50, 3);
    Eigen::VectorXd y(50);
    std::mt19937 gen(42);
    std::normal_distribution<> d(0.0, 1.0);
    for (int i = 0; i < 50; ++i) {
        X(i, 0) = d(gen);
        X(i, 1) = d(gen);
        X(i, 2) = d(gen);
        y(i) = 2.0 * X(i, 0) + 1.0 * X(i, 1) + d(gen) * 0.1;
    }

    auto result = lasso.fit(X, y);
    ASSERT_TRUE(result.is_ok());

    const auto& info = lasso.get_convergence_info();
    EXPECT_GT(info.iterations, 0);
    EXPECT_TRUE(info.converged);
    EXPECT_EQ(info.termination_reason, "tolerance");
}

TEST_F(ConvergenceMonitoringTest, MarkovSwitchingGetConvergenceInfoConsistency) {
    MarkovSwitchingConfig config;
    config.n_states = 2;
    config.max_iterations = 50;
    MarkovSwitching ms(config);

    auto fit_result = ms.fit(ms_data_);
    ASSERT_TRUE(fit_result.is_ok());

    const auto& ms_result = fit_result.value();
    const auto& info = ms.get_convergence_info();

    EXPECT_EQ(info.iterations, ms_result.n_iterations);
    EXPECT_EQ(info.converged, ms_result.converged);
    EXPECT_FALSE(info.objective_history.empty());
}

TEST_F(ConvergenceMonitoringTest, ConvergenceInfoSummaryReturnsNonEmpty) {
    ConvergenceInfo info;
    info.iterations = 10;
    info.converged = true;
    info.termination_reason = "tolerance";
    info.final_tolerance = 1e-7;

    std::string summary = info.summary();
    EXPECT_FALSE(summary.empty());
    EXPECT_NE(summary.find("Converged"), std::string::npos);
    EXPECT_NE(summary.find("10"), std::string::npos);

    ConvergenceInfo info2;
    info2.iterations = 100;
    info2.converged = false;
    info2.termination_reason = "max_iterations";

    std::string summary2 = info2.summary();
    EXPECT_FALSE(summary2.empty());
    EXPECT_NE(summary2.find("Did not converge"), std::string::npos);
}

TEST_F(ConvergenceMonitoringTest, ConvergenceInfoIsSuccessfulLogic) {
    ConvergenceInfo info;
    info.converged = true;
    info.termination_reason = "tolerance";
    EXPECT_TRUE(info.is_successful());

    info.converged = true;
    info.termination_reason = "max_iterations";
    EXPECT_FALSE(info.is_successful());

    info.converged = false;
    info.termination_reason = "tolerance";
    EXPECT_FALSE(info.is_successful());
}

TEST_F(ConvergenceMonitoringTest, UnfittedModelReturnsDefaultConvergenceInfo) {
    GARCHConfig garch_config;
    GARCH garch(garch_config);
    const auto& info = garch.get_convergence_info();
    EXPECT_EQ(info.iterations, 0);
    EXPECT_FALSE(info.converged);
    EXPECT_TRUE(info.objective_history.empty());

    HMMConfig hmm_config;
    HMM hmm(hmm_config);
    const auto& hmm_info = hmm.get_convergence_info();
    EXPECT_EQ(hmm_info.iterations, 0);
    EXPECT_FALSE(hmm_info.converged);
}

TEST_F(ConvergenceMonitoringTest, EGARCHConvergenceInfoWorks) {
    EGARCHConfig config;
    EGARCH egarch(config);

    auto result = egarch.fit_with_diagnostics(returns_);
    ASSERT_TRUE(result.is_ok());

    const auto& info = result.value();
    EXPECT_GT(info.iterations, 0);
    EXPECT_FALSE(info.objective_history.empty());
}

TEST_F(ConvergenceMonitoringTest, GJRGARCHConvergenceInfoWorks) {
    GJRGARCHConfig config;
    GJRGARCH gjr(config);

    auto result = gjr.fit_with_diagnostics(returns_);
    ASSERT_TRUE(result.is_ok());

    const auto& info = result.value();
    EXPECT_GT(info.iterations, 0);
    EXPECT_FALSE(info.objective_history.empty());
}
