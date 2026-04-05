#include "trade_ngin/statistics/state_estimation/ms_dfm.hpp"
#include <gtest/gtest.h>
#include <random>
#include <cmath>

using namespace trade_ngin::statistics;

// ============================================================================
// Synthetic regime-switching factor data
// ============================================================================

static Eigen::MatrixXd make_regime_data(int T, int K, int J,
                                         unsigned seed = 42)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::uniform_real_distribution<double> ud(0.0, 1.0);

    // Regime-dependent parameters
    std::vector<Eigen::MatrixXd> A(J), Q(J);
    for (int j = 0; j < J; ++j) {
        double persistence = 0.8 + 0.05 * j;
        double vol = 0.1 + 0.5 * j;
        A[j] = Eigen::MatrixXd::Identity(K, K) * persistence;
        Q[j] = Eigen::MatrixXd::Identity(K, K) * vol;
    }

    // Transition matrix: high persistence
    double persist = 0.97;
    Eigen::MatrixXd P = Eigen::MatrixXd::Constant(J, J, (1.0 - persist) / (J - 1));
    for (int j = 0; j < J; ++j) P(j, j) = persist;

    // Generate data
    Eigen::MatrixXd factors(T, K);
    factors.row(0).setZero();
    int regime = 0;

    for (int t = 1; t < T; ++t) {
        // Regime transition
        double u = ud(rng);
        double cum = 0.0;
        for (int j = 0; j < J; ++j) {
            cum += P(regime, j);
            if (u < cum) { regime = j; break; }
        }

        // Factor dynamics
        Eigen::VectorXd noise(K);
        for (int k = 0; k < K; ++k) noise(k) = nd(rng);

        Eigen::LLT<Eigen::MatrixXd> llt(Q[regime]);
        Eigen::VectorXd f_new = A[regime] * factors.row(t - 1).transpose() +
                                 llt.matrixL() * noise;
        factors.row(t) = f_new.transpose();
    }

    return factors;
}

// ============================================================================
// Tests
// ============================================================================

class MSDFMTest : public ::testing::Test {
protected:
    Eigen::MatrixXd factors_;
    void SetUp() override {
        factors_ = make_regime_data(300, 3, 2);
    }
};

TEST_F(MSDFMTest, FitRuns) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;
    cfg.regime_labels = {"calm", "stress"};

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error()) << result.error()->what();
    EXPECT_TRUE(msdfm.is_fitted());
}

TEST_F(MSDFMTest, OutputDimensions) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    EXPECT_EQ(out.T, 300);
    EXPECT_EQ(out.K, 3);
    EXPECT_EQ(out.J, 2);
    EXPECT_EQ(out.smoothed_probs.rows(), 300);
    EXPECT_EQ(out.smoothed_probs.cols(), 2);
    EXPECT_EQ(out.filtered_probs.rows(), 300);
    EXPECT_EQ(out.filtered_probs.cols(), 2);
    EXPECT_EQ(out.transition_matrix.rows(), 2);
    EXPECT_EQ(out.transition_matrix.cols(), 2);
    EXPECT_EQ(static_cast<int>(out.regime_signatures.size()), 2);
    EXPECT_EQ(static_cast<int>(out.decoded_regimes.size()), 300);
}

TEST_F(MSDFMTest, RegimeSeparation) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 100;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    // Regimes should be ordered by volatility: regime 0 < regime 1
    EXPECT_LT(out.regime_signatures[0].mean_volatility,
              out.regime_signatures[1].mean_volatility);
}

TEST_F(MSDFMTest, TransitionMatrixValid) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    for (int i = 0; i < out.J; ++i) {
        double row_sum = out.transition_matrix.row(i).sum();
        EXPECT_NEAR(row_sum, 1.0, 1e-6) << "Row " << i << " doesn't sum to 1";
        for (int j = 0; j < out.J; ++j) {
            EXPECT_GE(out.transition_matrix(i, j), 0.0);
            EXPECT_LE(out.transition_matrix(i, j), 1.0);
        }
        // Diagonal should be > 0.5 (persistent)
        EXPECT_GT(out.transition_matrix(i, i), 0.5);
    }
}

TEST_F(MSDFMTest, SmoothedProbsSumToOne) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    for (int t = 0; t < out.T; ++t) {
        double row_sum = out.smoothed_probs.row(t).sum();
        EXPECT_NEAR(row_sum, 1.0, 1e-6) << "Smoothed probs at t=" << t;
    }
}

TEST_F(MSDFMTest, ErgodicProbsValid) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    EXPECT_NEAR(out.ergodic_probs.sum(), 1.0, 1e-6);
    for (int j = 0; j < out.J; ++j) {
        EXPECT_GT(out.ergodic_probs(j), 0.0);
    }
}

TEST_F(MSDFMTest, FitFromDFMOutput) {
    // Build a DFMOutput manually
    DFMOutput dfm_out;
    dfm_out.T = 300;
    dfm_out.K = 3;
    dfm_out.N = 5;
    dfm_out.factors.resize(300, std::vector<double>(3));
    for (int t = 0; t < 300; ++t)
        for (int k = 0; k < 3; ++k)
            dfm_out.factors[t][k] = factors_(t, k);

    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 30;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(dfm_out);
    ASSERT_FALSE(result.is_error());
    EXPECT_EQ(result.value().T, 300);
}

TEST_F(MSDFMTest, OnlineUpdate) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;

    MarkovSwitchingDFM msdfm(cfg);
    msdfm.fit(factors_);

    std::mt19937 rng(99);
    std::normal_distribution<double> nd(0.0, 1.0);

    for (int i = 0; i < 10; ++i) {
        Eigen::VectorXd f(3);
        for (int k = 0; k < 3; ++k) f(k) = nd(rng);

        auto upd = msdfm.update(f);
        ASSERT_FALSE(upd.is_error()) << upd.error()->what();
        EXPECT_EQ(upd.value().size(), 2);
        EXPECT_NEAR(upd.value().sum(), 1.0, 1e-6);
        EXPECT_TRUE(upd.value().allFinite());
    }
}

TEST_F(MSDFMTest, ThreeRegimes) {
    auto factors3 = make_regime_data(500, 3, 3, 123);

    MSDFMConfig cfg;
    cfg.n_regimes = 3;
    cfg.max_em_iterations = 100;
    cfg.regime_labels = {"expansion", "slowdown", "stress"};

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors3);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    EXPECT_EQ(out.J, 3);
    EXPECT_EQ(static_cast<int>(out.regime_signatures.size()), 3);

    // Each regime should have non-trivial occupation
    for (int j = 0; j < 3; ++j) {
        double occupation = out.smoothed_probs.col(j).sum() / out.T;
        EXPECT_GT(occupation, 0.02) << "Regime " << j << " has near-zero occupation";
    }
}

TEST_F(MSDFMTest, ConvergenceInfoPopulated) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 100;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    EXPECT_GT(out.convergence_info.iterations, 0);
    EXPECT_FALSE(out.convergence_info.objective_history.empty());
    EXPECT_FALSE(out.convergence_info.termination_reason.empty());
}

TEST_F(MSDFMTest, LogLikelihoodMonotonicity) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 100;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& hist = result.value().convergence_info.objective_history;
    ASSERT_GE(static_cast<int>(hist.size()), 2);
    for (size_t i = 1; i < hist.size(); ++i) {
        EXPECT_GE(hist[i], hist[i - 1] - 1e-6)
            << "LL decreased at iter " << i << ": " << hist[i] << " < " << hist[i-1];
    }
}

TEST_F(MSDFMTest, FilteredProbsSumToOne) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 50;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors_);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    for (int t = 0; t < out.T; ++t) {
        double row_sum = out.filtered_probs.row(t).sum();
        EXPECT_NEAR(row_sum, 1.0, 1e-6) << "Filtered probs at t=" << t;
    }
}

TEST_F(MSDFMTest, DecodedAccuracyOnSyntheticData) {
    // Generate data with well-separated regimes for easier recovery
    int T = 500;
    int K = 2;
    std::mt19937 rng(77);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::uniform_real_distribution<double> ud(0.0, 1.0);

    Eigen::MatrixXd factors(T, K);
    factors.row(0).setZero();
    std::vector<int> true_regimes(T, 0);

    // Regime 0: low vol (0.05), Regime 1: high vol (2.0)
    Eigen::MatrixXd A0 = Eigen::MatrixXd::Identity(K, K) * 0.9;
    Eigen::MatrixXd A1 = Eigen::MatrixXd::Identity(K, K) * 0.5;
    double vol0 = 0.05, vol1 = 2.0;

    int regime = 0;
    for (int t = 1; t < T; ++t) {
        // 3% switch probability
        if (ud(rng) < 0.03) regime = 1 - regime;
        true_regimes[t] = regime;

        double vol = (regime == 0) ? vol0 : vol1;
        Eigen::MatrixXd A = (regime == 0) ? A0 : A1;
        Eigen::VectorXd noise(K);
        for (int k = 0; k < K; ++k) noise(k) = nd(rng) * std::sqrt(vol);
        factors.row(t) = (A * factors.row(t-1).transpose() + noise).transpose();
    }

    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 100;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors);
    ASSERT_FALSE(result.is_error());

    auto& out = result.value();
    // Count correct matches (regime 0 = low vol in both true and estimated)
    int correct = 0;
    for (int t = 0; t < T; ++t) {
        if (out.decoded_regimes[t] == true_regimes[t]) ++correct;
    }
    double accuracy = static_cast<double>(correct) / T;
    EXPECT_GT(accuracy, 0.60)
        << "Decoded accuracy " << accuracy << " is too low";
}

TEST(MSDFMErrorTest, UpdateDimensionMismatch) {
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 30;

    MarkovSwitchingDFM msdfm(cfg);
    msdfm.fit(make_regime_data(100, 3, 2));

    // Pass wrong dimension
    Eigen::VectorXd f(5);  // should be 3
    f.setOnes();
    auto result = msdfm.update(f);
    EXPECT_TRUE(result.is_error());
}

TEST(MSDFMEdgeTest, ConstantInputHandled) {
    // All-constant factors — should not crash, should produce valid probs
    Eigen::MatrixXd factors = Eigen::MatrixXd::Constant(100, 3, 1.0);
    // Add tiny noise to avoid exact singularity
    std::mt19937 rng(42);
    std::normal_distribution<double> nd(0.0, 1e-8);
    for (int t = 0; t < 100; ++t)
        for (int k = 0; k < 3; ++k)
            factors(t, k) += nd(rng);

    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    cfg.max_em_iterations = 20;

    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(factors);
    // Should either succeed with valid output or fail gracefully
    if (!result.is_error()) {
        auto& out = result.value();
        for (int t = 0; t < out.T; ++t) {
            EXPECT_NEAR(out.smoothed_probs.row(t).sum(), 1.0, 1e-4);
            EXPECT_TRUE(out.smoothed_probs.row(t).allFinite());
        }
    }
}

TEST(MSDFMErrorTest, TooFewObservations) {
    auto small = make_regime_data(10, 3, 2);
    MSDFMConfig cfg;
    cfg.n_regimes = 2;
    MarkovSwitchingDFM msdfm(cfg);
    auto result = msdfm.fit(small);
    EXPECT_TRUE(result.is_error());
}

TEST(MSDFMErrorTest, UpdateBeforeFit) {
    MarkovSwitchingDFM msdfm;
    Eigen::VectorXd f(3);
    f << 1.0, 2.0, 3.0;
    auto result = msdfm.update(f);
    EXPECT_TRUE(result.is_error());
}

TEST(MSDFMConfigTest, JsonRoundTrip) {
    MSDFMConfig cfg;
    cfg.n_regimes = 4;
    cfg.em_tol = 1e-4;
    cfg.transition_persistence = 0.9;
    cfg.regime_labels = {"a", "b", "c", "d"};

    MSDFMConfig cfg2;
    cfg2.from_json(cfg.to_json());

    EXPECT_EQ(cfg2.n_regimes, 4);
    EXPECT_EQ(cfg2.em_tol, 1e-4);
    EXPECT_EQ(cfg2.transition_persistence, 0.9);
    EXPECT_EQ(cfg2.regime_labels, (std::vector<std::string>{"a", "b", "c", "d"}));
}
